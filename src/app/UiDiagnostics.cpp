#include "UiDiagnostics.hpp"
#include <chrono>
#include <cstdio>
#include <sys/stat.h>
#include <unistd.h>
#include <cstring>

#if defined(MIYOOFIN_ENABLE_PERF_TELEMETRY) && MIYOOFIN_ENABLE_PERF_TELEMETRY == 1
#include "../diagnostics/PerformanceTelemetry.hpp"
#endif

namespace miyoofin {
#if defined(MIYOOFIN_ENABLE_PERF_TELEMETRY) && MIYOOFIN_ENABLE_PERF_TELEMETRY == 1
namespace {

ActionId actionIdFromDiagnosticName(const char *action) noexcept
{
    if (action == nullptr)
        return ActionId::Other;
    if (std::strcmp(action, "None") == 0)
        return ActionId::None;
    if (std::strcmp(action, "Up") == 0)
        return ActionId::Up;
    if (std::strcmp(action, "Down") == 0)
        return ActionId::Down;
    if (std::strcmp(action, "Left") == 0)
        return ActionId::Left;
    if (std::strcmp(action, "Right") == 0)
        return ActionId::Right;
    if (std::strcmp(action, "Confirm (A)") == 0)
        return ActionId::Confirm;
    if (std::strcmp(action, "Back (B)") == 0)
        return ActionId::Back;
    if (std::strcmp(action, "Search (X)") == 0)
        return ActionId::Search;
    if (std::strcmp(action, "Actions (Y)") == 0)
        return ActionId::ActionsMenu;
    if (std::strcmp(action, "PrevTab (L1)") == 0)
        return ActionId::PrevTab;
    if (std::strcmp(action, "NextTab (R1)") == 0)
        return ActionId::NextTab;
    if (std::strcmp(action, "PrevPage (L2)") == 0)
        return ActionId::PrevPage;
    if (std::strcmp(action, "NextPage (R2)") == 0)
        return ActionId::NextPage;
    if (std::strcmp(action, "Settings (START)") == 0)
        return ActionId::Settings;
    if (std::strcmp(action, "Menu (SELECT)") == 0)
        return ActionId::Menu;
    if (std::strcmp(action, "Exit (MENU)") == 0)
        return ActionId::Exit;
    if (std::strcmp(action, "Raw") == 0)
        return ActionId::Raw;
    return ActionId::Other;
}

void emitUiStall(StallEdge edge, const UiDiagnostics &diagnostics,
                 uint64_t durationUs) noexcept
{
    PerformanceTelemetry &telemetry = performanceTelemetry();
    if (!telemetry.enabledFast())
        return;
    TelemetryRecord record{};
    record.header.record_type = RecordType::UiStall;
    record.payload.ui_stall.edge = static_cast<uint8_t>(edge);
    record.payload.ui_stall.screen = static_cast<uint16_t>(
        PerformanceTelemetry::screenIdFromDiagnosticName(
            diagnostics.screenName()));
    record.payload.ui_stall.tab = static_cast<uint16_t>(
        PerformanceTelemetry::tabIdFromDiagnosticName(diagnostics.tabName()));
    record.payload.ui_stall.action = static_cast<uint16_t>(
        actionIdFromDiagnosticName(diagnostics.actionName()));
    record.payload.ui_stall.phase = static_cast<uint8_t>(
        UiDiagnostics::phaseIdFromDiagnosticName(diagnostics.phaseName()));
    record.payload.ui_stall.duration_us = durationUs;
    record.payload.ui_stall.scope_id = static_cast<uint16_t>(
        UiDiagnostics::scopeIdFromDiagnosticName(diagnostics.scopeName()));
    record.payload.ui_stall.worker_mask = diagnostics.activeWorkerMask();
    telemetry.emitRecord(record);
}

} // namespace
#endif
uint64_t UiDiagnostics::monotonicMs() { return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
int UiDiagnostics::Watchdog::poll(uint64_t heartbeat, uint64_t now, bool suspended, uint64_t &duration) { duration=0; if(suspended){seen=heartbeat;stalled=false;return 0;} if(heartbeat!=seen){int r=stalled?2:0; if(stalled) duration=now-seen; seen=heartbeat; stalled=false; return r;} if(!stalled && now>=seen+STALL_MS){stalled=true;return 1;} return 0; }
UiDiagnostics::UiDiagnostics(){} UiDiagnostics::~UiDiagnostics(){stop();}
void UiDiagnostics::start(const std::string &path){ if(m_thread.joinable())return; m_path=path; m_stop=false; heartbeat(); event("diagnostics started"); m_thread=std::thread(&UiDiagnostics::watchdogLoop,this); }
void UiDiagnostics::stop(){ m_stop=true; if(m_thread.joinable())m_thread.join(); }
void UiDiagnostics::heartbeat(){m_heartbeat.store(monotonicMs(),std::memory_order_relaxed);}
UiPhaseId UiDiagnostics::phaseIdFromDiagnosticName(const char *phase) noexcept
{
    if (phase == nullptr)
        return UiPhaseId::Unknown;
    if (std::strcmp(phase, "idle/frame boundary") == 0)
        return UiPhaseId::IdleFrameBoundary;
    if (std::strcmp(phase, "event/input") == 0)
        return UiPhaseId::EventInput;
    if (std::strcmp(phase, "update") == 0)
        return UiPhaseId::Update;
    if (std::strcmp(phase, "screen transition") == 0)
        return UiPhaseId::ScreenTransition;
    if (std::strcmp(phase, "render") == 0)
        return UiPhaseId::Render;
    return UiPhaseId::Unknown;
}

UiScopeId UiDiagnostics::scopeIdFromDiagnosticName(const char *scope) noexcept
{
    if (scope == nullptr)
        return UiScopeId::Unknown;
    if (std::strcmp(scope, "Screen::handleAction") == 0)
        return UiScopeId::ScreenHandleAction;
    if (std::strcmp(scope, "Screen::update") == 0)
        return UiScopeId::ScreenUpdate;
    if (std::strcmp(scope, "App::finishSavedSessionValidation") == 0)
        return UiScopeId::AppFinishSavedSessionValidation;
    if (std::strcmp(scope, "Screen::render") == 0)
        return UiScopeId::ScreenRender;
    if (std::strcmp(scope, "ScreenStack::push -> null") == 0)
        return UiScopeId::ScreenStackPushNull;
    if (std::strcmp(scope, "ScreenStack::push -> MovieDetailsScreen") == 0)
        return UiScopeId::ScreenStackPushMovieDetailsScreen;
    if (std::strcmp(scope, "ScreenStack::push -> SeriesScreen") == 0)
        return UiScopeId::ScreenStackPushSeriesScreen;
    if (std::strcmp(scope, "ScreenStack::push -> EpisodeBrowserScreen") == 0)
        return UiScopeId::ScreenStackPushEpisodeBrowserScreen;
    if (std::strcmp(scope, "ScreenStack::push -> HomeScreen") == 0)
        return UiScopeId::ScreenStackPushHomeScreen;
    if (std::strcmp(scope, "ScreenStack::push -> Screen") == 0)
        return UiScopeId::ScreenStackPushScreen;
    if (std::strcmp(scope, "ScreenStack::pop") == 0)
        return UiScopeId::ScreenStackPop;
    if (std::strcmp(scope, "ScreenStack::signalLeave") == 0)
        return UiScopeId::ScreenStackSignalLeave;
    if (std::strcmp(scope, "ScreenStack::retireScreen") == 0)
        return UiScopeId::ScreenStackRetireScreen;
    if (std::strcmp(scope, "ScreenStack::enterPrevious") == 0)
        return UiScopeId::ScreenStackEnterPrevious;
    if (std::strcmp(scope, "HomeScreen::publishLibraryResult") == 0)
        return UiScopeId::HomeScreenPublishLibraryResult;
    if (std::strcmp(scope, "HomeScreen::publishResumeResult") == 0)
        return UiScopeId::HomeScreenPublishResumeResult;
    if (std::strcmp(scope, "HomeScreen::updateArtworkWorkingSet") == 0)
        return UiScopeId::HomeScreenUpdateArtworkWorkingSet;
    if (std::strcmp(scope, "HomeScreen::publishDecodedArtwork") == 0)
        return UiScopeId::HomeScreenPublishDecodedArtwork;
    if (std::strcmp(scope, "HomeScreen::queueSelectedArtwork") == 0)
        return UiScopeId::HomeScreenQueueSelectedArtwork;
    if (std::strcmp(scope, "HomeScreen::queueVisibleArtwork") == 0)
        return UiScopeId::HomeScreenQueueVisibleArtwork;
    if (std::strcmp(scope, "HomeScreen::publishDownloadSnapshot") == 0)
        return UiScopeId::HomeScreenPublishDownloadSnapshot;
    if (std::strcmp(scope, "EpisodeBrowserScreen::enter") == 0)
        return UiScopeId::EpisodeBrowserScreenEnter;
    if (std::strcmp(scope, "EpisodeBrowserScreen::workerShutdown") == 0)
        return UiScopeId::EpisodeBrowserScreenWorkerShutdown;
    if (std::strcmp(scope, "EpisodeBrowserScreen::publishEpisodes") == 0)
        return UiScopeId::EpisodeBrowserScreenPublishEpisodes;
    if (std::strcmp(scope, "EpisodeBrowserScreen::publishArtworkResult") == 0)
        return UiScopeId::EpisodeBrowserScreenPublishArtworkResult;
    if (std::strcmp(scope, "MovieDetailsScreen::enter") == 0)
        return UiScopeId::MovieDetailsScreenEnter;
    if (std::strcmp(scope, "MovieDetailsScreen::worker cancellation") == 0)
        return UiScopeId::MovieDetailsScreenWorkerCancellation;
    if (std::strcmp(scope, "MovieDetailsScreen::owned worker creation") == 0)
        return UiScopeId::MovieDetailsScreenOwnedWorkerCreation;
    if (std::strcmp(scope, "MovieDetailsScreen::publish async preparation") == 0)
        return UiScopeId::MovieDetailsScreenPublishAsyncPreparation;
    if (std::strcmp(scope, "MovieDetailsScreen::publish image surface preparation") == 0)
        return UiScopeId::MovieDetailsScreenPublishImageSurfacePreparation;
    if (std::strcmp(scope, "MovieDetailsScreen::publish playback/download state") == 0)
        return UiScopeId::MovieDetailsScreenPublishPlaybackDownloadState;
    if (std::strcmp(scope, "DownloadManager::planSnapshot mutex wait") == 0)
        return UiScopeId::DownloadManagerPlanSnapshotMutexWait;
    return UiScopeId::Unknown;
}
void UiDiagnostics::event(const char *message){std::lock_guard<std::mutex>l(m_eventsMutex); if(m_events.size()==EVENT_CAPACITY)m_events.erase(m_events.begin()); m_events.emplace_back(message);}
void UiDiagnostics::setWorker(const char *worker,const char *state){std::atomic<const char*> *target=nullptr;uint16_t mask=0;if(!strcmp(worker,"library")){target=&m_library;mask=kWorkerMaskHomeLibraryFetch;}else if(!strcmp(worker,"hierarchy")){target=&m_hierarchy;mask=kWorkerMaskHomeHierarchy;}else if(!strcmp(worker,"artwork")){target=&m_artwork;mask=kWorkerMaskHomePoster;}else if(!strcmp(worker,"download")){target=&m_download;mask=kWorkerMaskDownloadTransfer;}if(target){target->store(state,std::memory_order_relaxed);const uint16_t active=state!=nullptr&&strcmp(state,"idle")!=0?mask:0;uint16_t observed=m_activeWorkerMask.load(std::memory_order_relaxed);for(;;){const uint16_t desired=(observed&~mask)|active;if(m_activeWorkerMask.compare_exchange_weak(observed,desired,std::memory_order_relaxed,std::memory_order_relaxed))break;}}}
std::vector<std::string> UiDiagnostics::recentEvents()const{std::lock_guard<std::mutex>l(m_eventsMutex);return m_events;}
void UiDiagnostics::writeLine(const std::string &line){ struct stat st{}; if(stat(m_path.c_str(),&st)==0 && st.st_size>65536){ FILE*f=fopen(m_path.c_str(),"w");if(f)fclose(f); } FILE*f=fopen(m_path.c_str(),"a"); if(!f && m_path!="ui-stall.log") { m_path="ui-stall.log"; f=fopen(m_path.c_str(),"a"); } if(f){fprintf(f,"%s\n",line.c_str());fclose(f);} }
void UiDiagnostics::slow(const char *name,uint64_t elapsed)
{
    if(elapsed<SLOW_MS)return;
    char b[256];snprintf(b,sizeof b,"[UISLOW] %llums %s",(unsigned long long)elapsed,name);
    event(b);
    {
        std::lock_guard<std::mutex>l(m_pendingMutex);
        m_pendingLogs.emplace_back(b);
    }
#if defined(MIYOOFIN_ENABLE_PERF_TELEMETRY) && MIYOOFIN_ENABLE_PERF_TELEMETRY == 1
    emitUiStall(StallEdge::SlowScope,*this,elapsed*1000ull);
#endif
}
UiDiagnostics::Scope::Scope(const char *name):Scope(name,true){}
UiDiagnostics::Scope::Scope(const char *name, bool trackUiScope)
    :m_name(name),m_previous(nullptr),m_start(UiDiagnostics::monotonicMs()),m_trackUiScope(trackUiScope)
{
    if(m_trackUiScope)m_previous=uiDiagnostics().exchangeScope(name);
}
UiDiagnostics::Scope::~Scope(){auto&e=uiDiagnostics(); if(m_trackUiScope)e.setScope(m_previous); e.slow(m_name,UiDiagnostics::monotonicMs()-m_start);}
void UiDiagnostics::watchdogLoop()
{
    Watchdog w;
    w.seen=m_heartbeat.load();
    while(!m_stop.load()) {
        {
            std::vector<std::string> logs;
            {
                std::lock_guard<std::mutex>l(m_pendingMutex);
                logs.swap(m_pendingLogs);
            }
            for(const auto &line:logs)writeLine(line);
        }
        const uint64_t now=monotonicMs();
        uint64_t d=0;
        int r=w.poll(m_heartbeat.load(),now,m_suspended.load(),d);
        if(r) {
            char b[768];
            if(r==1) {
                snprintf(b,sizeof b,"[UISTALL] begin stalled=%llums phase=%s screen=%s tab=%s action=%s scope=%s library=%s hierarchy=%s artwork=%s download=%s",(unsigned long long)(now-w.seen),m_phase.load(),m_screen.load(),m_tab.load(),m_action.load(),m_scope.load(),m_library.load(),m_hierarchy.load(),m_artwork.load(),m_download.load());
#if defined(MIYOOFIN_ENABLE_PERF_TELEMETRY) && MIYOOFIN_ENABLE_PERF_TELEMETRY == 1
                emitUiStall(StallEdge::Begin,*this,0);
#endif
            } else {
                snprintf(b,sizeof b,"[UISTALL] end duration=%llums",(unsigned long long)d);
#if defined(MIYOOFIN_ENABLE_PERF_TELEMETRY) && MIYOOFIN_ENABLE_PERF_TELEMETRY == 1
                emitUiStall(StallEdge::End,*this,d*1000ull);
#endif
            }
            event(b);
            writeLine(b);
            if(r==1) {
                for(const auto&e:recentEvents())writeLine("[UISTALL] recent "+e);
            }
        }
        usleep(100000);
    }
}
UiDiagnostics &uiDiagnostics(){static UiDiagnostics d;return d;}
}
