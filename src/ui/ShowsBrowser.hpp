#ifndef MIYOOFIN_SHOWS_BROWSER_HPP
#define MIYOOFIN_SHOWS_BROWSER_HPP
#include "TitleOrganization.hpp"
#include "../cache/LibraryCache.hpp"
#include <algorithm>
#include <map>
namespace miyoofin {
static constexpr int SHOWS_GRID_COLUMNS=4, SHOWS_GRID_ROWS=3;
inline bool asciiEqualsAnime(const std::string &s) { return asciiCaseInsensitiveCompare(s,"Anime")==0; }
inline bool libraryNameContainsAnimeToken(const std::string &s) { for(size_t i=0;i<s.size();) { while(i<s.size()&&!((s[i]>='A'&&s[i]<='Z')||(s[i]>='a'&&s[i]<='z')))++i; size_t b=i; while(i<s.size()&&((s[i]>='A'&&s[i]<='Z')||(s[i]>='a'&&s[i]<='z')))++i; if(i>b&&asciiCaseInsensitiveCompare(std::string_view(s).substr(b,i-b),"anime")==0)return true; } return false; }
inline bool isAnimeSeries(const CachedLibraryView &view,const MediaItem &item) { if(libraryNameContainsAnimeToken(view.name))return true; for(const auto &g:item.genres)if(asciiEqualsAnime(g))return true; return false; }
struct ShowsPresentation { std::vector<MediaItem> shows,anime; };
inline ShowsPresentation makeShowsPresentation(const std::vector<CachedLibraryView> &views) { std::map<std::string,size_t> normal,anime; ShowsPresentation out; for(const auto&v:views)for(const auto&i:v.items){bool a=isAnimeSeries(v,i); auto ni=normal.find(i.id),ai=anime.find(i.id); if(a){if(ai==anime.end()){if(ni!=normal.end()){out.shows.erase(out.shows.begin()+ni->second); normal.clear(); for(size_t n=0;n<out.shows.size();++n)normal[out.shows[n].id]=n;} anime[i.id]=out.anime.size();out.anime.push_back(i);}}else if(ni==normal.end()&&ai==anime.end()){normal[i.id]=out.shows.size();out.shows.push_back(i);}} std::sort(out.shows.begin(),out.shows.end(),organizationalLess);std::sort(out.anime.begin(),out.anime.end(),organizationalLess);return out; }
inline int moveShowsGrid(int index,int count,int dr,int dc){if(count<=0)return 0;index=std::max(0,std::min(index,count-1));int r=index/4+dr,c=index%4+dc;if(c<0||c>=4||r<0)return index;int t=r*4+c;return t>=count?(dr>0?count-1:index):t;}
inline int clampShowsGridScroll(int selected,int count,int scroll){if(count<=0)return 0;selected=std::max(0,std::min(selected,count-1));int row=selected/4,last=(count-1)/4,max=std::max(0,last-2);scroll=std::max(0,std::min(scroll,max));if(row<scroll)scroll=row;if(row>=scroll+3)scroll=row-2;return scroll;}
inline int closestShowsGridIndex(int source,int targetCount){if(targetCount<=0)return 0;int row=source/4,col=source%4;int t=row*4+col;return std::min(t,targetCount-1);}
// Cross a continuous eight-column Shows/Anime row.  Crossing always lands on
// the edge column of the target half, retaining the closest corresponding row.
inline int crossShowsGridIndex(int source, int targetCount, bool toAnime) {
    if (targetCount <= 0) return 0;
    int row = std::max(0, source) / 4;
    int target = row * 4 + (toAnime ? 0 : 3);
    int rowFirst = row * 4, rowLast = std::min(targetCount - 1, rowFirst + 3);
    if (rowFirst <= targetCount - 1) return std::max(rowFirst, std::min(target, rowLast));
    return targetCount - 1;
}
}
#endif
