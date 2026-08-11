#include "UiDiagnostics.hpp"
#include <chrono>
#include <cstdio>
#include <sys/stat.h>
#include <unistd.h>
#include <cstring>

namespace miyoofin {
uint64_t UiDiagnostics::monotonicMs() { return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
int UiDiagnostics::Watchdog::poll(uint64_t heartbeat, uint64_t now, bool suspended, uint64_t &duration) { duration=0; if(suspended){seen=heartbeat;stalled=false;return 0;} if(heartbeat!=seen){int r=stalled?2:0; if(stalled) duration=now-seen; seen=heartbeat; stalled=false; return r;} if(!stalled && now>=seen+STALL_MS){stalled=true;return 1;} return 0; }
UiDiagnostics::UiDiagnostics(){} UiDiagnostics::~UiDiagnostics(){stop();}
void UiDiagnostics::start(const std::string &path){ if(m_thread.joinable())return; m_path=path; m_stop=false; heartbeat(); event("diagnostics started"); m_thread=std::thread(&UiDiagnostics::watchdogLoop,this); }
void UiDiagnostics::stop(){ m_stop=true; if(m_thread.joinable())m_thread.join(); }
void UiDiagnostics::heartbeat(){m_heartbeat.store(monotonicMs(),std::memory_order_relaxed);}
void UiDiagnostics::event(const char *message){std::lock_guard<std::mutex>l(m_eventsMutex); if(m_events.size()==EVENT_CAPACITY)m_events.erase(m_events.begin()); m_events.emplace_back(message);}
void UiDiagnostics::setWorker(const char *worker,const char *state){std::atomic<const char*> *target=nullptr; if(!strcmp(worker,"library"))target=&m_library;else if(!strcmp(worker,"hierarchy"))target=&m_hierarchy;else if(!strcmp(worker,"artwork"))target=&m_artwork;else if(!strcmp(worker,"download"))target=&m_download;if(target)target->store(state,std::memory_order_relaxed);}
std::vector<std::string> UiDiagnostics::recentEvents()const{std::lock_guard<std::mutex>l(m_eventsMutex);return m_events;}
void UiDiagnostics::writeLine(const std::string &line){ struct stat st{}; if(stat(m_path.c_str(),&st)==0 && st.st_size>65536){ FILE*f=fopen(m_path.c_str(),"w");if(f)fclose(f); } FILE*f=fopen(m_path.c_str(),"a"); if(!f && m_path!="ui-stall.log") { m_path="ui-stall.log"; f=fopen(m_path.c_str(),"a"); } if(f){fprintf(f,"%s\n",line.c_str());fclose(f);} }
void UiDiagnostics::slow(const char *name,uint64_t elapsed){if(elapsed<SLOW_MS)return; char b[256];snprintf(b,sizeof b,"[UISLOW] %llums %s",(unsigned long long)elapsed,name);event(b);std::lock_guard<std::mutex>l(m_pendingMutex);m_pendingLogs.emplace_back(b);}
UiDiagnostics::Scope::Scope(const char *name):Scope(name,true){}
UiDiagnostics::Scope::Scope(const char *name, bool trackUiScope)
    :m_name(name),m_previous(nullptr),m_start(UiDiagnostics::monotonicMs()),m_trackUiScope(trackUiScope)
{
    if(m_trackUiScope)m_previous=uiDiagnostics().exchangeScope(name);
}
UiDiagnostics::Scope::~Scope(){auto&e=uiDiagnostics(); if(m_trackUiScope)e.setScope(m_previous); e.slow(m_name,UiDiagnostics::monotonicMs()-m_start);}
void UiDiagnostics::watchdogLoop(){Watchdog w;w.seen=m_heartbeat.load(); while(!m_stop.load()){ {std::vector<std::string> logs;{std::lock_guard<std::mutex>l(m_pendingMutex);logs.swap(m_pendingLogs);}for(const auto &line:logs)writeLine(line);} const uint64_t now=monotonicMs();uint64_t d=0;int r=w.poll(m_heartbeat.load(),now,m_suspended.load(),d);if(r){char b[768];if(r==1)snprintf(b,sizeof b,"[UISTALL] begin stalled=%llums phase=%s screen=%s tab=%s action=%s scope=%s library=%s hierarchy=%s artwork=%s download=%s",(unsigned long long)(now-w.seen),m_phase.load(),m_screen.load(),m_tab.load(),m_action.load(),m_scope.load(),m_library.load(),m_hierarchy.load(),m_artwork.load(),m_download.load());else snprintf(b,sizeof b,"[UISTALL] end duration=%llums",(unsigned long long)d);event(b);writeLine(b);if(r==1){for(const auto&e:recentEvents())writeLine("[UISTALL] recent "+e);}} usleep(100000);}}
UiDiagnostics &uiDiagnostics(){static UiDiagnostics d;return d;}
}
