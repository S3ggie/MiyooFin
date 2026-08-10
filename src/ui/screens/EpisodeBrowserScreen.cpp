#include "EpisodeBrowserScreen.hpp"
#include "../../download/DownloadSupport.hpp"
#include "../Theme.hpp"
#include "../BitmapFont.hpp"
#include "../../app/ScreenStack.hpp"
#include "../../net/JellyfinApi.hpp"
#include "../../net/ArtworkUrl.hpp"
#include "../../net/HttpClient.hpp"
#include "../../cache/ImageCache.hpp"
#include "../../cache/OfflineCatalog.hpp"
#include "../../cache/LibraryCache.hpp"
#include "../../playback/PlaybackRequest.hpp"
#include <cstdio>
#include <cstring>

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
static constexpr int LIST_VISIBLE   = 22;

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

// -------------------------------------------------------------------
// Bottom hint bar renderer (matches SeriesScreen)
// -------------------------------------------------------------------
static void renderBottomHints(SDL_Surface *fb, const char *hint)
{
    int y = FB_H - BOTTOM_H;
    BitmapFont::fillRect(fb, 0, y, FB_W, BOTTOM_H,
        Theme::BG_R * 2 / 3, Theme::BG_G * 2 / 3,
        Theme::BG_B * 2 / 3, 255);
    BitmapFont::drawString(fb, 8, y + 2, hint,
        Theme::TEXT_R, Theme::TEXT_G, Theme::TEXT_B,
        Theme::BG_R * 2 / 3, Theme::BG_G * 2 / 3,
        Theme::BG_B * 2 / 3);
}

// -------------------------------------------------------------------
// Format episode number prefix: "E01", "E02", etc.
// -------------------------------------------------------------------
static std::string formatEpNum(int indexNumber)
{
    if (indexNumber <= 0) return {};
    char buf[16];
    std::snprintf(buf, sizeof(buf), "E%02d", indexNumber);
    return std::string(buf);
}

// -------------------------------------------------------------------
// Constructor
// -------------------------------------------------------------------
EpisodeBrowserScreen::EpisodeBrowserScreen(const Session &session,
                                           const MediaItem &series,
                                           const MediaItem &season,
                                           const std::string &initialEpisodeId, std::shared_ptr<DownloadManager> downloads)
    : m_session(session)
    , m_series(series)
    , m_season(season)
    , m_initialEpisodeId(initialEpisodeId)
    , m_downloads(std::move(downloads))
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
    int selected, int total, const std::set<int> &unavailable)
{
    if (selected < 0 || selected >= total) return -1;
    if (!unavailable.count(selected)) return selected;
    for (int offset = 1; offset <= PREFETCH_AHEAD; ++offset) {
        const int index = selected + offset;
        if (index < total && !unavailable.count(index)) return index;
    }
    for (int offset = 1; offset <= PREFETCH_BEHIND; ++offset) {
        const int index = selected - offset;
        if (index >= 0 && !unavailable.count(index)) return index;
    }
    return -1;
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
    printf("[EpisodeBrowserScreen] enter series=%s season=%s\n",
           m_series.title.c_str(), m_season.title.c_str());
    if (m_episodes.empty() && m_loadState != LoadState::Error) {m_episodes=OfflineCatalog::episodes(OfflineCatalog::cachePath("cache",LibraryCache::scopeKey(m_session.serverUrl,m_session.userId)),m_season.id);m_loadState=LoadState::Ready;fetchEpisodes();}
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
void EpisodeBrowserScreen::fetchEpisodes()
{
    if(m_fetchThread.joinable()){std::lock_guard<std::mutex>g(m_fetchMutex);if(!m_fetchDone)return; m_fetchThread.join();} if(m_episodes.empty())m_loadState = LoadState::Loading;
    m_error.clear();
    {std::lock_guard<std::mutex>g(m_fetchMutex);m_fetchDone=false;} m_fetchCancelled.store(false, std::memory_order_release); Session s=m_session;std::string sid=m_series.id,season=m_season.id;
    m_fetchThread=std::thread([this,s,sid,season](){std::vector<MediaItem>v;std::string e;bool ok=JellyfinApi::getEpisodes(s.serverUrl,s.accessToken,s.userId,s.deviceId,sid,season,v,e,&m_fetchCancelled);std::lock_guard<std::mutex>g(m_fetchMutex);m_fetchOk=ok;m_fetchEpisodes=std::move(v);m_fetchError=e;m_fetchDone=true;});
    return;
    std::string error;
    bool ok = JellyfinApi::getEpisodes(
        m_session.serverUrl, m_session.accessToken,
        m_session.userId, m_session.deviceId,
        m_series.id, m_season.id, m_episodes, error);
    if (ok) {
        m_loadState = LoadState::Ready;
        m_selectedEpisode = 0;
        m_listScroll = 0;
        m_overviewScroll = 0;
        m_focus = FocusArea::EpisodeList;
        m_actionBtn = ActionButton::Play;
        m_episodeArtwork = {};
        m_episodeArtworkKey.clear();

        std::vector<ArtworkJob> artworkJobs;
        artworkJobs.reserve(m_episodes.size());
        for (const auto &ep : m_episodes) {
            ArtworkJob job;
            auto tag = ep.imageTags.find("Primary");
            if (tag != ep.imageTags.end() && !tag->second.empty()) {
                job.itemId = ep.id;
                job.imageTag = tag->second;
                job.artworkKey = ep.id + ":Primary:" + tag->second + ":288x162";
            }
            artworkJobs.push_back(job);
        }

        // Publish immutable scheduling data for the fresh episode list.
        {
            std::lock_guard<std::mutex> lock(m_workerMutex);
            m_workerHasCompletion = false;
            m_artworkJobs = std::move(artworkJobs);
            m_failedKeys.clear();
            m_workerPaused = false;
        }

        // B5e3b: Apply initial episode focus if requested
        if (!m_initialEpisodeId.empty() && !m_initialSelectionApplied) {
            int idx = findEpisodeIndex(m_episodes, m_initialEpisodeId);
            if (idx >= 0) {
                m_selectedEpisode = idx;
                printf("[EpisodeBrowserScreen] Initial focus: episode %d (id=%s)\n",
                       idx, m_initialEpisodeId.c_str());
            } else {
                printf("[EpisodeBrowserScreen] Initial focus: id '%s' not found, "
                       "using default\n", m_initialEpisodeId.c_str());
            }
            m_initialSelectionApplied = true;
        }

        clampListScroll();

        printf("[EpisodeBrowserScreen] Loaded %d episodes\n",
               (int)m_episodes.size());
        wakeArtworkWorker();
    } else {
        m_loadState = LoadState::Error;
        m_error = error.empty() ? "Unknown error" : error;
        printf("[EpisodeBrowserScreen] Failed to load episodes: %s\n",
               m_error.c_str());
    }
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
        if(action==Action::Confirm && m_downloads && m_planId){auto p=m_downloads->planSnapshot(m_planId);if(p.state==DownloadPlanState::Ready&&p.plan.canFit)m_downloads->enqueue(p.plan.items);m_confirmDownload=false;}
        return true;
    }
    // Y plans the whole displayed season.  Episodes have already been fetched,
    // so only the manager-owned planner performs network work from here.
    if (action == Action::ActionsMenu && m_downloads && !m_episodes.empty()) {
        if(m_planId && m_planIsSeason && m_downloads->planSnapshot(m_planId).state==DownloadPlanState::Ready) m_confirmDownload=true;
        else {m_confirmDownload=false; m_planIsSeason=true; m_planId=m_downloads->requestPlan(m_episodes);} return true;
    }

    // ----- EpisodeList focus -----
    if (m_focus == FocusArea::EpisodeList) {
        switch (action) {
        case Action::Up:
            if (m_selectedEpisode > 0) {
                m_selectedEpisode--;
                clampListScroll();
                m_overviewScroll = 0;
                wakeArtworkWorker();
            }
            return true;
        case Action::Down:
            if (m_selectedEpisode + 1 < total) {
                m_selectedEpisode++;
                clampListScroll();
                m_overviewScroll = 0;
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
                    printf("[EpisodeBrowserScreen] Play selected: %s\n",
                           m_episodes[m_selectedEpisode].title.c_str());
                    std::string error;
                    PlaybackSource source=m_downloads?resolvePlayback(m_episodes[m_selectedEpisode],*m_downloads):PlaybackSource::Jellyfin;
                    if (source==PlaybackSource::UnavailableOffline) return true;
                    if (PlaybackRequest::writeWithSourceTo(PlaybackRequest::defaultPath(),
                            m_episodes[m_selectedEpisode].id,
                            "episode",
                            m_episodes[m_selectedEpisode].playbackPositionTicks, source==PlaybackSource::Local?"local":"jellyfin", source==PlaybackSource::Local?m_downloads->scope():"",
                            error))
                    {
                        {
                            std::lock_guard<std::mutex> lock(m_workerMutex);
                            m_workerPaused = true;
                            ++m_workerGeneration;
                        }
                        m_prefetchResumePending = true;
                        m_prefetchResumeDelayUpdates = 1;
                        m_playbackEpisodeId = m_episodes[m_selectedEpisode].id;
                        printf("[EpisodeBrowserScreen] Playback request "
                               "written, requesting external playback\n");
                        m_stack->requestExternalPlayback();
                    } else {
                        printf("[EpisodeBrowserScreen] Playback request "
                               "failed: %s\n", error.c_str());
                    }
                }
            } else {
                if (m_confirmDownload) { if(m_downloads&&m_planId){auto p=m_downloads->planSnapshot(m_planId);if(p.state==DownloadPlanState::Ready&&p.plan.canFit)m_downloads->enqueue(p.plan.items);}m_confirmDownload=false; }
                else if(m_downloads && m_selectedEpisode>=0 && m_selectedEpisode<total) { bool season=m_actionBtn==ActionButton::DownloadSeason; if(m_planId && m_planIsSeason==season && m_downloads->planSnapshot(m_planId).state==DownloadPlanState::Ready) m_confirmDownload=true; else {m_planIsSeason=season;m_planId=m_downloads->requestPlan(season?m_episodes:std::vector<MediaItem>{m_episodes[m_selectedEpisode]});} }
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
    bool fetchDone; { std::lock_guard<std::mutex> g(m_fetchMutex); fetchDone=m_fetchDone; }
    if(fetchDone){std::vector<MediaItem>fresh;std::string err;bool ok;{std::lock_guard<std::mutex>g(m_fetchMutex);ok=m_fetchOk;fresh=std::move(m_fetchEpisodes);err=m_fetchError;m_fetchDone=false;}if(m_fetchThread.joinable())m_fetchThread.join();if(ok){std::string selected=m_episodes.empty()?"":m_episodes[m_selectedEpisode].id;m_episodes=std::move(fresh);int n=findEpisodeIndex(m_episodes,selected);m_selectedEpisode=n>=0?n:0;m_loadState=LoadState::Ready;m_listScroll=0;m_episodeArtwork={};m_episodeArtworkKey.clear();std::vector<ArtworkJob> jobs;for(const auto&ep:m_episodes){ArtworkJob j;j.itemId=ep.id;auto t=ep.imageTags.find("Primary");if(t!=ep.imageTags.end()&&!t->second.empty()){j.imageTag=t->second;j.artworkKey=ep.id+":Primary:"+t->second+":288x162";}jobs.push_back(j);} {std::lock_guard<std::mutex>g(m_workerMutex);m_artworkJobs=std::move(jobs);m_failedKeys.clear();m_workerPaused=false;}clampListScroll();OfflineCatalog::storeEpisodes(OfflineCatalog::cachePath("cache",LibraryCache::scopeKey(m_session.serverUrl,m_session.userId)),m_series,m_season,m_episodes,nullptr);wakeArtworkWorker();}else if(m_episodes.empty()){m_loadState=LoadState::Error;m_error=err;}}
    if (advancePrefetchResume(m_prefetchResumePending,
                              m_prefetchResumeDelayUpdates)) {
        {
            std::lock_guard<std::mutex> lock(m_workerMutex);
            m_workerPaused = false;
            ++m_workerGeneration;
        }
        m_workerCv.notify_one();

        std::int64_t resultTicks = 0;
        std::string error;
        if (PlaybackRequest::consumeResult(m_playbackEpisodeId,
                                           resultTicks, error)) {
            const int playedIndex = findEpisodeIndex(m_episodes,
                                                      m_playbackEpisodeId);
            if (playedIndex >= 0) {
                m_episodes[playedIndex].playbackPositionTicks = resultTicks;
                printf("[EpisodeBrowserScreen] Playback position updated: "
                       "%lld\n", (long long)resultTicks);
            }
        }
        m_playbackEpisodeId.clear();
    }

    if (m_loadState == LoadState::Ready)
        tryLoadSelectedEpisodeArtwork();
}

// -------------------------------------------------------------------
// tryLoadSelectedEpisodeArtwork — non-blocking (B5g1a)
// Main thread NEVER performs HTTP.  Selection changes are immediate
// for metadata.  Artwork uses cache-first + background worker.
// -------------------------------------------------------------------
void EpisodeBrowserScreen::tryLoadSelectedEpisodeArtwork()
{
    int total = (int)m_episodes.size();
    if (total <= 0 || m_selectedEpisode < 0 || m_selectedEpisode >= total) {
        m_episodeArtwork = {};
        m_episodeArtworkKey.clear();
        return;
    }

    const MediaItem &ep = m_episodes[m_selectedEpisode];

    // Look for Primary image tag (NOT Thumb)
    auto it = ep.imageTags.find("Primary");
    if (it == ep.imageTags.end() || it->second.empty()) {
        m_episodeArtwork = {};
        m_episodeArtworkKey = ep.id + ":none:288x162";
        return;
    }

    constexpr int w = 288;
    constexpr int h = 162;
    const std::string &tag = it->second;

    // Build stable identity key: episodeId:Primary:imageTag:288x162
    std::string key = ep.id + ":Primary:" + tag + ":288x162";

    // 1. Already decoded artwork for this exact key?  Done.
    if (m_episodeArtworkKey == key && !m_episodeArtwork.empty())
        return;

    // 2. Check worker completion signal (consumed once per frame max)
    {
        std::lock_guard<std::mutex> lock(m_workerMutex);
        if (m_workerHasCompletion) {
            ArtworkCompletion comp = m_workerCompletion;
            m_workerHasCompletion = false;
            if (comp.artworkKey == key) {
                if (comp.success) {
                    auto jpegData = ImageCache::readCached(
                        ep.id, ImageType::Primary, tag, w, h);
                    if (!jpegData.empty()) {
                        m_episodeArtwork = ImageDecoder::decodeJpeg(
                            jpegData.data(), jpegData.size());
                        if (!m_episodeArtwork.empty()) {
                            m_episodeArtworkKey = key;
                            printf("[EpisodeBrowserScreen] Artwork:"
                                   " decoded selected %s\n",
                                   ep.id.c_str());
                        }
                    }
                }
                return;
            }
            // Stale completion (old selection) — fall through
        }
    }

    // 3. Selection changed — clear previous decoded artwork
    if (m_episodeArtworkKey != key) {
        m_episodeArtwork = {};
        m_episodeArtworkKey = key;
    }

    // 4. Failed or already in progress?  Wait.
    {
        std::lock_guard<std::mutex> lock(m_workerMutex);
        if (m_failedKeys.count(key) || m_workerInProgressKey == key)
            return;
    }

    // 6. Check disk cache (fast filesystem-only, no network)
    if (ImageCache::isCached(ep.id, ImageType::Primary, tag, w, h)) {
        auto jpegData = ImageCache::readCached(
            ep.id, ImageType::Primary, tag, w, h);
        if (!jpegData.empty()) {
            m_episodeArtwork = ImageDecoder::decodeJpeg(
                jpegData.data(), jpegData.size());
            if (!m_episodeArtwork.empty()) {
                m_episodeArtworkKey = key;
                printf("[EpisodeBrowserScreen] Artwork:"
                       " cache hit, decoded %s\n", ep.id.c_str());
                return;
            }
        }
    }

    // 6. The worker owns all network scheduling; make sure it is awake.
    wakeArtworkWorker();
}

void EpisodeBrowserScreen::wakeArtworkWorker()
{
    {
        std::lock_guard<std::mutex> lock(m_workerMutex);
        m_workerSelected = m_selectedEpisode;
        ++m_workerGeneration;
        if (!m_workerThread.joinable())
            m_workerThread = std::thread(
                &EpisodeBrowserScreen::artworkWorkerLoop, this);
    }
    m_workerCv.notify_one();
}

// -------------------------------------------------------------------
// artworkWorkerLoop — background thread (B5g1a)
//
// Chooses one artwork job at a time from a freshly computed bounded window.
// Never accesses SDL, rendering, or m_episodeArtwork.
// -------------------------------------------------------------------
void EpisodeBrowserScreen::artworkWorkerLoop()
{
    std::uint64_t observedGeneration = 0;
    while (true) {
        ArtworkJob job;
        int candidate = -1;
        std::uint64_t generation = 0;
        {
            std::unique_lock<std::mutex> lock(m_workerMutex);
            m_workerCv.wait(lock, [&] {
                return m_workerStop || (!m_workerPaused &&
                    observedGeneration != m_workerGeneration);
            });
            if (m_workerStop) break;
            generation = m_workerGeneration;
            observedGeneration = generation;
        }

        std::set<int> unavailable;
        while (true) {
            int selected;
            {
                std::lock_guard<std::mutex> lock(m_workerMutex);
                if (m_workerStop) return;
                if (m_workerPaused || generation != m_workerGeneration) break;
                selected = m_workerSelected;
                candidate = nextPrefetchIndex(
                    selected, (int)m_artworkJobs.size(), unavailable);
                if (candidate < 0) break;
                job = m_artworkJobs[candidate];
                if (job.artworkKey.empty() ||
                    m_failedKeys.count(job.artworkKey) ||
                    m_workerInProgressKey == job.artworkKey) {
                    unavailable.insert(candidate);
                    continue;
                }
            }
            if (ImageCache::isCached(job.itemId, ImageType::Primary,
                                     job.imageTag, job.width, job.height)) {
                unavailable.insert(candidate);
                continue;
            }
            {
                std::lock_guard<std::mutex> lock(m_workerMutex);
                if (m_workerPaused || generation != m_workerGeneration) break;
                m_workerInProgressKey = job.artworkKey;
            }
            printf("[EpisodeBrowserScreen] Prefetch: selected=%d candidate=%d\n",
                   selected, candidate);
            break;
        }
        if (candidate < 0) {
            printf("[EpisodeBrowserScreen] Prefetch: window warm\n");
            continue;
        }
        {
            std::lock_guard<std::mutex> lock(m_workerMutex);
            if (m_workerInProgressKey != job.artworkKey) continue;
        }

        // --- Execute job (no lock held) ---
        bool success = false;

        if (ImageCache::isCached(job.itemId, ImageType::Primary,
                                 job.imageTag, job.width, job.height))
        {
            success = true;
        } else {
            std::string url = buildImageUrl(
                m_session.serverUrl, job.itemId, ImageType::Primary,
                job.imageTag, job.width, job.height);

            HttpClient client;
            client.setTimeoutSec(3);
            auto headers = JellyfinApi::buildAuthHeaders(
                m_session.accessToken, m_session.deviceId);

            BinaryHttpResponse response;
            std::string error;
            if (client.getBinary(url, headers, response, error,
                                 512 * 1024, &m_workerCancelled))
            {
                if (response.ok()) {
                    const bool cacheWriteSucceeded = ImageCache::writeToCache(
                        job.itemId, ImageType::Primary, job.imageTag,
                        job.width, job.height,
                        response.data.data(), response.data.size());
                    if (cacheWriteSucceeded) {
                        printf("[EpisodeBrowserScreen] ArtworkWorker:"
                               " downloaded episode=%s\n", job.itemId.c_str());
                        success = true;
                    } else {
                        printf("[EpisodeBrowserScreen] ArtworkWorker:"
                               " cache write failed\n");
                    }
                } else {
                    printf("[EpisodeBrowserScreen] ArtworkWorker:"
                           " HTTP %ld\n", response.status);
                }
            } else {
                printf("[EpisodeBrowserScreen] ArtworkWorker:"
                       " network error: %s\n", error.c_str());
            }
        }

        // --- Publish completion signal (under lock) ---
        {
            std::lock_guard<std::mutex> lock(m_workerMutex);
            m_workerInProgressKey.clear();
            if (!m_workerStop) {
                if (!success) m_failedKeys.insert(job.artworkKey);
                m_workerCompletion = {job.artworkKey, success};
                m_workerHasCompletion = true;
            }
        }
        // Recompute from the current selection after every request.
        {
            std::lock_guard<std::mutex> lock(m_workerMutex);
            ++m_workerGeneration;
            observedGeneration = m_workerGeneration - 1;
        }
    }
}

// -------------------------------------------------------------------
// render
// -------------------------------------------------------------------
void EpisodeBrowserScreen::render(SDL_Surface *fb)
{
    // --- Loading state ---
    if (m_loadState == LoadState::Loading) {
        BitmapFont::drawString(fb, LEFT_X, HEAD_Y, "EPISODES",
            Theme::ACCENT_R, Theme::ACCENT_G, Theme::ACCENT_B,
            Theme::BG_R, Theme::BG_G, Theme::BG_B);
        const char *msg = "Loading episodes...";
        int mx = (FB_W - (int)std::strlen(msg) * BitmapFont::GLYPH_W) / 2;
        BitmapFont::drawString(fb, mx, FB_H / 3, msg,
            Theme::ACCENT_R, Theme::ACCENT_G, Theme::ACCENT_B,
            Theme::BG_R, Theme::BG_G, Theme::BG_B);
        renderBottomHints(fb, "B=Back");
        return;
    }
    // --- Error state ---
    if (m_loadState == LoadState::Error) {
        BitmapFont::drawString(fb, LEFT_X, HEAD_Y, "EPISODES",
            Theme::ACCENT_R, Theme::ACCENT_G, Theme::ACCENT_B,
            Theme::BG_R, Theme::BG_G, Theme::BG_B);
        const char *hdr = "Failed to load episodes";
        int hx = (FB_W - (int)std::strlen(hdr) * BitmapFont::GLYPH_W) / 2;
        BitmapFont::drawString(fb, hx, FB_H / 3, hdr,
            Theme::HIGHLIGHT_R, Theme::HIGHLIGHT_G, Theme::HIGHLIGHT_B,
            Theme::BG_R, Theme::BG_G, Theme::BG_B);
        char errBuf[128];
        std::snprintf(errBuf, sizeof(errBuf), "%s", m_error.c_str());
        int maxC = (FB_W - 32) / BitmapFont::GLYPH_W;
        if ((int)std::strlen(errBuf) > maxC) errBuf[maxC] = '\0';
        int ex = (FB_W - (int)std::strlen(errBuf) * BitmapFont::GLYPH_W) / 2;
        BitmapFont::drawString(fb, ex, FB_H / 3 + BitmapFont::GLYPH_H + 8,
            errBuf, Theme::TEXT_R, Theme::TEXT_G, Theme::TEXT_B,
            Theme::BG_R, Theme::BG_G, Theme::BG_B);
        renderBottomHints(fb, "A=Retry  B=Back");
        return;
    }

    // === Ready: left panel ===
    int total = (int)m_episodes.size();
    BitmapFont::drawString(fb, LEFT_X, HEAD_Y, "EPISODES",
        Theme::ACCENT_R, Theme::ACCENT_G, Theme::ACCENT_B,
        Theme::BG_R, Theme::BG_G, Theme::BG_B);

    // --- Left panel: episode list rows ---
    for (int vis = 0; vis < LIST_VISIBLE; ++vis) {
        int idx = m_listScroll + vis;
        if (idx >= total) break;
        int ry = LIST_Y + vis * LIST_ROW_H;
        const auto &ep = m_episodes[idx];

        std::string epNum = formatEpNum(ep.indexNumber);
        std::string label = epNum.empty()
            ? ep.title : epNum + "  " + ep.title;

        int maxChars = LIST_W / BitmapFont::GLYPH_W;
        if ((int)label.size() > maxChars) {
            if (maxChars > 3) {
                label[maxChars - 1] = '.';
                label[maxChars - 2] = '.';
                label[maxChars - 3] = '.';
            }
            label.resize(maxChars > 0 ? maxChars : 1);
        }

        bool isSel = (idx == m_selectedEpisode);
        bool isFocused = (m_focus == FocusArea::EpisodeList && isSel);

        if (isFocused) {
            BitmapFont::fillRect(fb, LEFT_X - 2, ry - 1,
                LIST_W + 4, LIST_ROW_H, FOCUS_OR, FOCUS_OG, FOCUS_OB, 255);
            BitmapFont::drawString(fb, LEFT_X, ry, label.c_str(),
                0, 0, 0, FOCUS_OR, FOCUS_OG, FOCUS_OB);
        } else if (isSel) {
            BitmapFont::fillRect(fb, LEFT_X - 2, ry - 1,
                LIST_W + 4, LIST_ROW_H,
                Theme::ACCENT_R / 2, Theme::ACCENT_G / 2,
                Theme::ACCENT_B / 2, 255);
            BitmapFont::drawString(fb, LEFT_X, ry, label.c_str(),
                Theme::TEXT_R, Theme::TEXT_G, Theme::TEXT_B,
                Theme::ACCENT_R / 2, Theme::ACCENT_G / 2,
                Theme::ACCENT_B / 2);
        } else {
            BitmapFont::drawString(fb, LEFT_X, ry, label.c_str(),
                Theme::TEXT_R, Theme::TEXT_G, Theme::TEXT_B,
                Theme::BG_R, Theme::BG_G, Theme::BG_B);
        }
    }

    if (m_listScroll > 0)
        BitmapFont::drawString(fb, LEFT_X + LIST_W - 16, LIST_Y - 2,
            "^", Theme::ACCENT_R, Theme::ACCENT_G, Theme::ACCENT_B,
            Theme::BG_R, Theme::BG_G, Theme::BG_B);
    if (m_listScroll + LIST_VISIBLE < total)
        BitmapFont::drawString(fb, LEFT_X + LIST_W - 16,
            LIST_Y + LIST_VISIBLE * LIST_ROW_H,
            "v", Theme::ACCENT_R, Theme::ACCENT_G, Theme::ACCENT_B,
            Theme::BG_R, Theme::BG_G, Theme::BG_B);

    // === Right panel ===
    if (total <= 0 || m_selectedEpisode < 0 || m_selectedEpisode >= total) {
        const char *msg = "No episodes";
        int mx = (FB_W - (int)std::strlen(msg) * BitmapFont::GLYPH_W) / 2;
        BitmapFont::drawString(fb, mx, FB_H / 3, msg,
            Theme::TEXT_R, Theme::TEXT_G, Theme::TEXT_B,
            Theme::BG_R, Theme::BG_G, Theme::BG_B);
        renderBottomHints(fb, "B=Back");
        return;
    }

    const auto &ep = m_episodes[m_selectedEpisode];

    if (m_downloads && m_planId) {
        auto p=m_downloads->planSnapshot(m_planId); std::string status;
        if(m_confirmDownload) status=std::string("Download ")+(m_planIsSeason?"Season "+std::to_string(m_season.indexNumber):"Episode")+"?";
        else if(p.state==DownloadPlanState::Planning) status=(p.plan.sizeKnown?"~"+formatBytes(p.plan.additionalRequiredBytes)+"  Preparing download...":"Preparing download...");
        else if(p.state==DownloadPlanState::Ready) status=std::to_string(p.itemCount)+" episodes  ~"+formatBytes(p.plan.additionalRequiredBytes)+" needed  "+formatBytes(p.plan.usableFreeBytes)+" free";
        else if(p.state==DownloadPlanState::Error) status=p.plan.error;
        if(!status.empty()) BitmapFont::drawString(fb,META_X,BTN_Y-28,status.c_str(),Theme::ACCENT_R,Theme::ACCENT_G,Theme::ACCENT_B,Theme::BG_R,Theme::BG_G,Theme::BG_B);
        if(m_confirmDownload) BitmapFont::drawString(fb,META_X,BTN_Y-14,"A=Confirm  B=Cancel",Theme::TEXT_R,Theme::TEXT_G,Theme::TEXT_B,Theme::BG_R,Theme::BG_G,Theme::BG_B);
    }

    // Placeholder thumbnail (coloured box as fallback/background)
    BitmapFont::fillRect(fb, THUMB_X, THUMB_Y, THUMB_W, THUMB_H,
        ep.artR, ep.artG, ep.artB, 255);

    // Render decoded episode artwork if available, aspect-fit centred
    if (!m_episodeArtwork.empty()) {
        int imgW = m_episodeArtwork.width;
        int imgH = m_episodeArtwork.height;
        float imgAspect = (float)imgW / (float)imgH;
        float boxAspect = (float)THUMB_W / (float)THUMB_H;
        int drawW, drawH;
        if (imgAspect > boxAspect) {
            // Wider than box — fit to width
            drawW = THUMB_W;
            drawH = (int)(THUMB_W / imgAspect + 0.5f);
            if (drawH > THUMB_H) drawH = THUMB_H;
        } else {
            // Taller than box — fit to height
            drawH = THUMB_H;
            drawW = (int)(THUMB_H * imgAspect + 0.5f);
            if (drawW > THUMB_W) drawW = THUMB_W;
        }
        int drawX = THUMB_X + (THUMB_W - drawW) / 2;
        int drawY = THUMB_Y + (THUMB_H - drawH) / 2;

        // Create an SDL surface wrapping the RGBA pixel data
        SDL_Surface *imgSurface = SDL_CreateRGBSurfaceFrom(
            (void *)m_episodeArtwork.pixels.data(),
            imgW, imgH,
            32,                    // bits per pixel
            imgW * 4,              // pitch (bytes per row)
            0x000000FF,            // R mask
            0x0000FF00,            // G mask
            0x00FF0000,            // B mask
            0xFF000000);           // A mask
        if (imgSurface) {
            SDL_Rect srcRect = {0, 0, imgW, imgH};
            SDL_Rect dstRect = {drawX, drawY, drawW, drawH};
            SDL_BlitScaled(imgSurface, &srcRect, fb, &dstRect);
            SDL_FreeSurface(imgSurface);
        }
    }

    // Thumbnail border (drawn after artwork)
    BitmapFont::drawRect(fb, THUMB_X, THUMB_Y, THUMB_W, THUMB_H,
        Theme::TEXT_R, Theme::TEXT_G, Theme::TEXT_B);

    // Episode title
    int my = META_Y;
    std::string titleStr = ep.title;
    int maxTC = (FB_W - META_X - 8) / BitmapFont::GLYPH_W;
    if ((int)titleStr.size() > maxTC) {
        if (maxTC > 3) {
            titleStr[maxTC-1]='.';
            titleStr[maxTC-2]='.';
            titleStr[maxTC-3]='.';
        }
        titleStr.resize(maxTC);
    }
    BitmapFont::drawString(fb, META_X, my, titleStr.c_str(),
        Theme::ACCENT_R, Theme::ACCENT_G, Theme::ACCENT_B,
        Theme::BG_R, Theme::BG_G, Theme::BG_B);
    my += BitmapFont::GLYPH_H + 2;

    // Metadata: "S1 E3 | 11 min | 8.4/10"
    bool hasMeta = false;
    char metaBuf[128]; metaBuf[0] = '\0';
    if (ep.parentIndexNumber > 0 && ep.indexNumber > 0) {
        std::snprintf(metaBuf, sizeof(metaBuf), "S%d E%d",
                      ep.parentIndexNumber, ep.indexNumber);
        hasMeta = true;
    }
    if (ep.runTimeTicks > 0) {
        int mins = ticksToMinutes(ep.runTimeTicks);
        int len = (int)std::strlen(metaBuf);
        if (hasMeta)
            std::snprintf(metaBuf+len, sizeof(metaBuf)-len,
                          " | %d min", mins);
        else { std::snprintf(metaBuf, sizeof(metaBuf), "%d min", mins); hasMeta = true; }
    }
    if (ep.rating > 0.0f) {
        int len = (int)std::strlen(metaBuf);
        if (hasMeta)
            std::snprintf(metaBuf+len, sizeof(metaBuf)-len,
                          " | %.1f/10", (double)ep.rating);
        else
            std::snprintf(metaBuf, sizeof(metaBuf), "%.1f/10", (double)ep.rating);
    }
    if (hasMeta) {
        BitmapFont::drawString(fb, META_X, my, metaBuf,
            Theme::TEXT_R, Theme::TEXT_G, Theme::TEXT_B,
            Theme::BG_R, Theme::BG_G, Theme::BG_B);
        my += BitmapFont::GLYPH_H + 2;
    }

    // Overview (word-wrapped, scrollable)
    bool overviewScrollable = false;
    if (!ep.overview.empty()) {
        auto lines = wrapText(ep.overview.c_str(), META_WRAP);
        int vis = (BTN_Y - 4 - my) / BitmapFont::GLYPH_H;
        if (vis < 1) vis = 1;
        int maxScroll = (int)lines.size() - vis;
        if (maxScroll < 0) maxScroll = 0;
        if (m_overviewScroll > maxScroll) m_overviewScroll = maxScroll;
        if (m_overviewScroll < 0) m_overviewScroll = 0;
        overviewScrollable = (maxScroll > 0);
        int drawY = my;
        for (int i = m_overviewScroll;
             i < m_overviewScroll + vis && i < (int)lines.size(); ++i) {
            BitmapFont::drawString(fb, META_X, drawY, lines[i].c_str(),
                Theme::TEXT_R, Theme::TEXT_G, Theme::TEXT_B,
                Theme::BG_R, Theme::BG_G, Theme::BG_B);
            drawY += BitmapFont::GLYPH_H;
        }
    }

    // --- Action buttons ---
    auto drawBtn = [&](int bx, const char *label, bool focused) {
        if (focused) {
            BitmapFont::fillRect(fb, bx - 1, BTN_Y - 1,
                BTN_W + 2, BTN_H + 2, FOCUS_OR, FOCUS_OG, FOCUS_OB, 255);
            BitmapFont::drawRect(fb, bx - 2, BTN_Y - 2,
                BTN_W + 4, BTN_H + 4, FOCUS_OR, FOCUS_OG, FOCUS_OB);
            BitmapFont::drawRect(fb, bx - 1, BTN_Y - 1,
                BTN_W + 2, BTN_H + 2, FOCUS_IR, FOCUS_IG, FOCUS_IB);
        } else {
            BitmapFont::fillRect(fb, bx, BTN_Y, BTN_W, BTN_H,
                Theme::BG_R * 2 / 3, Theme::BG_G * 2 / 3,
                Theme::BG_B * 2 / 3, 255);
            BitmapFont::drawRect(fb, bx, BTN_Y, BTN_W, BTN_H,
                Theme::TEXT_R, Theme::TEXT_G, Theme::TEXT_B);
        }
        int tw = (int)std::strlen(label) * BitmapFont::GLYPH_W;
        int tx = bx + (BTN_W - tw) / 2;
        int ty = BTN_Y + (BTN_H - BitmapFont::GLYPH_H) / 2;
        BitmapFont::drawString(fb, tx, ty, label,
            Theme::TEXT_R, Theme::TEXT_G, Theme::TEXT_B,
            focused ? FOCUS_OR : (Theme::BG_R * 2 / 3),
            focused ? FOCUS_OG : (Theme::BG_G * 2 / 3),
            focused ? FOCUS_OB : (Theme::BG_B * 2 / 3));
    };

    bool playFocused = (m_focus == FocusArea::ActionButtons
                        && m_actionBtn == ActionButton::Play);
    bool dlFocused = (m_focus == FocusArea::ActionButtons
                      && m_actionBtn == ActionButton::DownloadEpisode);
    bool seasonFocused = (m_focus == FocusArea::ActionButtons
                      && m_actionBtn == ActionButton::DownloadSeason);
    drawBtn(BTN_PLAY_X, "PLAY", playFocused);
    drawBtn(BTN_EP_X, "EPISODE", dlFocused);
    drawBtn(BTN_SEASON_X, "SEASON", seasonFocused);

    // Bottom hint bar
    renderBottomHints(fb, overviewScrollable
        ? "A=Select B=Back Y=Season L/R=Bio" : "A=Select B=Back Y=Season");
}

} // namespace miyoofin
