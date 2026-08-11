#include "OfflineLibraryProjection.hpp"
#include "../ui/MovieTitle.hpp"
#include <algorithm>

namespace miyoofin {
namespace {
bool completeState(DownloadState s) { return s==DownloadState::Complete || s==DownloadState::LocalOnly || s==DownloadState::UpdateAvailable; }
void unique(std::vector<MediaItem> &v) { std::set<std::string> seen; v.erase(std::remove_if(v.begin(),v.end(),[&](const MediaItem&i){return i.id.empty()||!seen.insert(i.id).second;}),v.end()); }
}
OfflineLibraryProjection::OfflineLibraryProjection(const LibrarySnapshot &library, const OfflineCatalogSnapshot &catalog, const DownloadSnapshot &downloads) : m_library(library),m_catalog(catalog) {
    for(const auto &d:downloads.items) if(completeState(d.state) && !d.itemId.empty()) m_complete.insert(d.itemId);
    for(const auto &v:library.movies) for(const auto &i:v.items) if(!i.id.empty()) m_media[i.id]=i;
    for(const auto &i:library.continueWatching) if(!i.id.empty()) m_media[i.id]=i;
    for(const auto &i:library.recentlyAdded) if(!i.id.empty()) m_media[i.id]=i;
    m_series=catalog.series;
    for(const auto &v:library.shows) for(const auto &s:v.items) if(!s.id.empty()) { m_series[s.id]=s; m_media[s.id]=s; }
    for(const auto &a:catalog.seasonsBySeries) for(auto s:a.second) { s.seriesId=a.first; m_seasons[s.id]=std::move(s); }
    m_episodes=catalog.episodesBySeason;
    for(const auto &a:m_episodes) for(const auto &i:a.second) if(!i.id.empty()) m_media[i.id]=i;
    // Download metadata is a durable fallback for a complete episode whose
    // hierarchy cache predates it.
    for(const auto &d:downloads.items) if(completeState(d.state) && d.itemType=="episode") {
        MediaItem ep; ep.id=d.itemId; ep.type="episode"; ep.title=d.title.empty()?"Episode "+std::to_string(d.episodeNumber):d.title; ep.seriesId=d.seriesId; ep.seriesName=d.seriesName; ep.seasonId=d.seasonId; ep.indexNumber=d.episodeNumber; ep.parentIndexNumber=d.seasonNumber; ep.playbackPositionTicks=d.playbackPositionTicks; ep.runTimeTicks=d.runtimeTicks;
        if(!ep.seasonId.empty()) m_episodes[ep.seasonId].push_back(ep);
        m_media[ep.id]=ep;
        if(!ep.seasonId.empty() && !m_seasons.count(ep.seasonId)) { MediaItem s; s.id=ep.seasonId;s.seriesId=ep.seriesId;s.type="season";s.indexNumber=ep.parentIndexNumber;s.title=d.seasonName.empty()?"Season "+std::to_string(ep.parentIndexNumber):d.seasonName;m_seasons[s.id]=s; }
        if(!ep.seriesId.empty() && !m_series.count(ep.seriesId)) { MediaItem s;s.id=ep.seriesId;s.type="show";s.title=ep.seriesName.empty()?"Downloaded Show":ep.seriesName;m_series[s.id]=s; }
    }
    for(auto &a:m_episodes) unique(a.second);
}
bool OfflineLibraryProjection::playable(const std::string &id) const { return m_complete.count(id)!=0; }
std::vector<MediaItem> OfflineLibraryProjection::movies() const { std::vector<MediaItem> out; for(const auto &v:m_library.movies) for(const auto&i:v.items) if(playable(i.id)) out.push_back(i); unique(out); std::sort(out.begin(),out.end(),movieOrganizationalLess); return out; }
std::vector<MediaItem> OfflineLibraryProjection::episodes(const std::string &seasonId) const { std::vector<MediaItem> out; auto it=m_episodes.find(seasonId); if(it!=m_episodes.end()) for(const auto&i:it->second) if(playable(i.id)) out.push_back(i); unique(out); std::sort(out.begin(),out.end(),[](const MediaItem&a,const MediaItem&b){return a.indexNumber==b.indexNumber?a.title<b.title:a.indexNumber<b.indexNumber;}); return out; }
std::vector<MediaItem> OfflineLibraryProjection::seasons(const std::string &seriesId) const { std::vector<MediaItem> out; for(const auto &a:m_seasons) if(a.second.seriesId==seriesId&&!episodes(a.first).empty()) out.push_back(a.second); unique(out); std::sort(out.begin(),out.end(),[](const MediaItem&a,const MediaItem&b){return a.indexNumber==b.indexNumber?a.title<b.title:a.indexNumber<b.indexNumber;}); return out; }
std::vector<MediaItem> OfflineLibraryProjection::series() const { std::vector<MediaItem> out; for(const auto&a:m_series) if(!seasons(a.first).empty()) out.push_back(a.second); unique(out); std::sort(out.begin(),out.end(),[](const MediaItem&a,const MediaItem&b){return a.title<b.title;}); return out; }
}
