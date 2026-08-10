#ifndef MIYOOFIN_DOWNLOAD_UI_HPP
#define MIYOOFIN_DOWNLOAD_UI_HPP

#include "DownloadTypes.hpp"
#include <algorithm>

namespace miyoofin {
enum class DownloadPrimaryControl { None, Play, Pause, Resume, Retry };
inline bool downloadIsLocal(const DownloadItem &i) { return i.state==DownloadState::Complete || i.state==DownloadState::LocalOnly || i.state==DownloadState::UpdateAvailable; }
inline DownloadPrimaryControl downloadPrimaryControl(DownloadState s) { switch(s) { case DownloadState::Complete: case DownloadState::LocalOnly: case DownloadState::UpdateAvailable:return DownloadPrimaryControl::Play; case DownloadState::Queued: case DownloadState::Downloading:return DownloadPrimaryControl::Pause; case DownloadState::Paused: case DownloadState::PausedForPlayback:return DownloadPrimaryControl::Resume; case DownloadState::Failed: case DownloadState::WaitingForNetwork: case DownloadState::ServerMissing:return DownloadPrimaryControl::Retry; default:return DownloadPrimaryControl::None; } }
inline const char *downloadPrimaryControlLabel(DownloadPrimaryControl c) { switch(c) { case DownloadPrimaryControl::Play:return "Play"; case DownloadPrimaryControl::Pause:return "Pause"; case DownloadPrimaryControl::Resume:return "Resume"; case DownloadPrimaryControl::Retry:return "Retry"; default:return ""; } }
inline bool downloadCanRemove(const DownloadItem &i) { return !i.itemId.empty(); }
inline bool downloadRemoveIsDelete(const DownloadItem &i) { return downloadIsLocal(i); }
inline bool downloadRemovalConfirmed(const std::string &armedItemId, const DownloadItem &i) { return downloadCanRemove(i) && armedItemId == i.itemId; }
inline int clampDownloadSelection(int selected, int count) { return count<=0 ? 0 : std::max(0,std::min(selected,count-1)); }
inline int clampDownloadScroll(int selected,int count,int scroll,int visible) { if(count<=0||visible<=0)return 0;selected=clampDownloadSelection(selected,count);scroll=std::max(0,std::min(scroll,std::max(0,count-visible)));if(selected<scroll)scroll=selected;if(selected>=scroll+visible)scroll=selected-visible+1;return scroll; }
}
#endif
