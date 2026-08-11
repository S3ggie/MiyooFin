#ifndef MIYOOFIN_DOWNLOAD_HIERARCHY_HPP
#define MIYOOFIN_DOWNLOAD_HIERARCHY_HPP

#include "DownloadTypes.hpp"
#include "DownloadUi.hpp"
#include "DownloadSupport.hpp"
#include <algorithm>
#include <cctype>
#include <map>
#include <set>

namespace miyoofin {

// Snapshot-only presentation model for Home's Downloads tab.  It deliberately
// contains no store or worker access, so rebuilding it is safe on the UI path.
enum class DownloadHierarchyRowKind { Movie, Series, Season, Episode };
struct DownloadAggregate {
    size_t episodes=0, complete=0, active=0;
    std::uint64_t bytes=0, totalBytes=0;
    bool bytesKnown=false, progressKnown=false;
    unsigned progress=0;
};
struct DownloadHierarchyRow {
    DownloadHierarchyRowKind kind=DownloadHierarchyRowKind::Movie;
    std::string id, title;
    int indent=0;
    const DownloadItem *item=nullptr; // leaf only; points into snapshot items
    DownloadAggregate aggregate;
    bool expanded=false;
};
struct DownloadHierarchy {
    std::vector<DownloadHierarchyRow> movies, shows, visible;
};

// Y is owned by the Downloads tab for every visible row.  Leaves may use it
// for removal, while hierarchy parents deliberately consume it as a no-op.
inline bool downloadHierarchyConsumesActionsMenu(const DownloadHierarchyRow &) {
    return true;
}

inline std::string downloadHierarchySortTitle(std::string value) {
    std::string lower; lower.reserve(value.size());
    for (unsigned char c : value) lower.push_back((char)std::tolower(c));
    for (const char *article : {"the ", "an ", "a "})
        if (lower.rfind(article, 0) == 0) return lower.substr(std::char_traits<char>::length(article));
    return lower;
}
inline bool downloadHierarchyComplete(const DownloadItem &i) {
    return i.state==DownloadState::Complete || i.state==DownloadState::LocalOnly ||
           i.state==DownloadState::UpdateAvailable;
}
inline DownloadAggregate downloadHierarchyAggregate(const std::vector<const DownloadItem *> &items) {
    DownloadAggregate out; out.episodes=items.size(); out.bytesKnown=!items.empty();
    unsigned long long progressSum=0;
    for (const DownloadItem *p : items) {
        const DownloadItem &i=*p;
        if (downloadHierarchyComplete(i)) ++out.complete;
        if (i.state==DownloadState::Downloading) ++out.active;
        out.bytes=saturatingAdd(out.bytes,i.downloadedBytes);
        // Incomplete HLS expected bytes are a planning estimate, not a real
        // aggregate denominator.  Completed local HLS bytes are real.
        const bool known=!i.hlsStorage || downloadHierarchyComplete(i);
        if (!known || !displayDownloadBytes(i)) out.bytesKnown=false;
        else out.totalBytes=saturatingAdd(out.totalBytes,displayDownloadBytes(i));
        progressSum+=downloadPercent(i); // child progress is the worker's real progress
    }
    out.progressKnown=!items.empty();
    if (out.progressKnown) out.progress=(unsigned)(progressSum/items.size());
    return out;
}

inline std::string downloadHierarchySeriesKey(const DownloadItem &i) {
    return !i.seriesId.empty() ? i.seriesId : "title:"+i.seriesName;
}
inline std::string downloadHierarchySeasonKey(const DownloadItem &i) {
    return !i.seasonId.empty() ? i.seasonId : "number:"+std::to_string(i.seasonNumber)+":"+i.seasonName;
}

inline bool downloadHierarchyIsBulkRemovalParent(const DownloadHierarchyRow &row) {
    return row.kind==DownloadHierarchyRowKind::Series || row.kind==DownloadHierarchyRowKind::Season;
}

// Builds a local-only removal plan from the manager snapshot.  This has no
// Jellyfin/API dependency: callers pass these IDs only to DownloadManager::erase.
inline std::vector<std::string> downloadHierarchyBulkRemovalItemIds(
    const DownloadHierarchyRow &row, const DownloadSnapshot &snapshot) {
    std::vector<std::string> ids;
    if (!downloadHierarchyIsBulkRemovalParent(row) || row.id.rfind("series:",0)!=0) return ids;
    const size_t seasonAt=row.id.find("/season:");
    const std::string seriesRowId=seasonAt==std::string::npos ? row.id : row.id.substr(0,seasonAt);
    const std::string seasonKey=seasonAt==std::string::npos ? "" : row.id.substr(seasonAt+8);
    std::set<std::string> seen;
    for (const DownloadItem &item : snapshot.items) {
        if (item.itemType!="episode" || item.itemId.empty()) continue;
        if ("series:"+downloadHierarchySeriesKey(item)!=seriesRowId) continue;
        if (!seasonKey.empty() && downloadHierarchySeasonKey(item)!=seasonKey) continue;
        if (seen.insert(item.itemId).second) ids.push_back(item.itemId);
    }
    return ids;
}

inline bool downloadHierarchyBulkRemovalConfirmed(const std::string &armedRowId,
                                                   const DownloadHierarchyRow &row) {
    return downloadHierarchyIsBulkRemovalParent(row) && armedRowId==row.id;
}

inline DownloadHierarchy buildDownloadHierarchy(const DownloadSnapshot &snapshot,
                                                const std::set<std::string> &expanded) {
    DownloadHierarchy out;
    struct Season { std::string key, name; int number=0; std::vector<const DownloadItem *> episodes; };
    struct Series { std::string key, name; std::map<std::string, Season> seasons; };
    std::map<std::string, Series> series;
    for (const DownloadItem &i : snapshot.items) {
        if (i.itemType!="episode") {
            out.movies.push_back({DownloadHierarchyRowKind::Movie,"movie:"+i.itemId,i.title,0,&i,{}});
            continue;
        }
        const std::string sk=downloadHierarchySeriesKey(i), sek=downloadHierarchySeasonKey(i);
        Series &s=series[sk]; s.key=sk; if(s.name.empty()) s.name=i.seriesName.empty()?"Unknown Series":i.seriesName;
        Season &season=s.seasons[sek]; season.key=sek; season.number=i.seasonNumber;
        if(season.name.empty()) season.name=i.seasonName.empty()?(i.seasonNumber>0?"Season "+std::to_string(i.seasonNumber):"Unknown Season"):i.seasonName;
        season.episodes.push_back(&i);
    }
    std::sort(out.movies.begin(),out.movies.end(),[](const auto&a,const auto&b){ return a.item->createdAt==b.item->createdAt ? downloadHierarchySortTitle(a.title)<downloadHierarchySortTitle(b.title) : a.item->createdAt>b.item->createdAt; });
    std::vector<Series *> ordered; for(auto &entry:series) ordered.push_back(&entry.second);
    std::sort(ordered.begin(),ordered.end(),[](const Series*a,const Series*b){return downloadHierarchySortTitle(a->name)<downloadHierarchySortTitle(b->name);});
    for (Series *s : ordered) {
        std::vector<const DownloadItem *> all; for(auto &e:s->seasons) all.insert(all.end(),e.second.episodes.begin(),e.second.episodes.end());
        const std::string sid="series:"+s->key; DownloadHierarchyRow parent{DownloadHierarchyRowKind::Series,sid,s->name,0,nullptr,downloadHierarchyAggregate(all),expanded.count(sid)!=0};
        out.shows.push_back(parent);
        std::vector<Season *> seasons; for(auto &e:s->seasons) seasons.push_back(&e.second);
        std::sort(seasons.begin(),seasons.end(),[](const Season*a,const Season*b){if(a->number!=b->number) return a->number<b->number; return downloadHierarchySortTitle(a->name)<downloadHierarchySortTitle(b->name);});
        for (Season *season : seasons) {
            std::sort(season->episodes.begin(),season->episodes.end(),[](const DownloadItem*a,const DownloadItem*b){if(a->episodeNumber!=b->episodeNumber)return a->episodeNumber<b->episodeNumber;return downloadHierarchySortTitle(a->title)<downloadHierarchySortTitle(b->title);});
            const std::string seasonId=sid+"/season:"+season->key; DownloadHierarchyRow child{DownloadHierarchyRowKind::Season,seasonId,season->name,1,nullptr,downloadHierarchyAggregate(season->episodes),expanded.count(seasonId)!=0};
            if(parent.expanded) out.shows.push_back(child);
            if(parent.expanded && child.expanded) for(const DownloadItem *episode:season->episodes)
                out.shows.push_back({DownloadHierarchyRowKind::Episode,"episode:"+episode->itemId,episode->title,2,episode,{}});
        }
    }
    out.visible=out.movies; out.visible.insert(out.visible.end(),out.shows.begin(),out.shows.end());
    return out;
}

inline int downloadHierarchySelection(const std::vector<DownloadHierarchyRow> &rows,
                                      const std::string &id, int previous) {
    for(int n=0;n<(int)rows.size();++n) if(rows[n].id==id) return n;
    return clampDownloadSelection(previous,(int)rows.size());
}
}
#endif
