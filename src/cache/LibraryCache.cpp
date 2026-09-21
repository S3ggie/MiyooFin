#include "LibraryCache.hpp"
#include "../data/CatalogPrimitives.hpp"
#include <cstdio>
#include <cstdint>
#include <cerrno>
#include <sys/stat.h>
#include <unistd.h>
#include <map>
#include <cstring>

namespace miyoofin {
namespace {
constexpr uint32_t VERSION=3, MAX_STRING=1024*1024, MAX_ITEMS=100000, MAX_VIEWS=1024;
bool get32(const std::vector<unsigned char>&b,size_t&p,uint32_t&n){if(p+4>b.size())return false;n=0;for(int i=0;i<4;i++)n|=(uint32_t)b[p++]<<(i*8);return true;}
bool get64(const std::vector<unsigned char>&b,size_t&p,uint64_t&n){if(p+8>b.size())return false;n=0;for(int i=0;i<8;i++)n|=(uint64_t)b[p++]<<(i*8);return true;}
bool gs(const std::vector<unsigned char>&b,size_t&p,std::string&s){uint32_t n;if(!get32(b,p,n)||n>MAX_STRING||p+n>b.size())return false;s.assign((const char*)&b[p],n);p+=n;return true;}
bool gi(const std::vector<unsigned char>&b,size_t&p,MediaItem&i){uint32_t n,r;uint64_t q;if(!gs(b,p,i.id)||!gs(b,p,i.title)||!gs(b,p,i.overview)||!get32(b,p,n)||!get32(b,p,r)||!gs(b,p,i.genre)||!gs(b,p,i.type)||!gs(b,p,i.etag))return false;i.year=(int)n;std::memcpy(&i.rating,&r,4);if(!get32(b,p,n)||!get32(b,p,r)||!get64(b,p,q))return false;i.played=n!=0;std::memcpy(&i.progress,&r,4);i.playbackPositionTicks=(long long)q;if(!get32(b,p,n)||n>MAX_ITEMS)return false;for(uint32_t k=0;k<n;k++){std::string s;if(!gs(b,p,s))return false;i.genres.push_back(s);}if(!get32(b,p,n)||n>MAX_ITEMS)return false;for(uint32_t k=0;k<n;k++){std::string a,c;if(!gs(b,p,a)||!gs(b,p,c))return false;i.imageTags[a]=c;}if(!get32(b,p,n))return false;i.indexNumber=(int)n;if(!get32(b,p,n)||!get64(b,p,q)||!gs(b,p,i.seriesName)||!gs(b,p,i.seriesId)||!gs(b,p,i.seasonId)||p+3>b.size())return false;i.parentIndexNumber=(int)n;i.runTimeTicks=(long long)q;i.placeholderArtwork.red=b[p++];i.placeholderArtwork.green=b[p++];i.placeholderArtwork.blue=b[p++];return true;}
bool gm(const std::vector<unsigned char>&b,size_t&p,std::vector<MediaItem>&vs){uint32_t n;if(!get32(b,p,n)||n>MAX_ITEMS)return false;for(uint32_t i=0;i<n;++i){MediaItem item;if(!gi(b,p,item))return false;vs.push_back(std::move(item));}return true;}
bool gv(const std::vector<unsigned char>&b,size_t&p,std::vector<CachedLibraryView>&vs){
 uint32_t viewCount;if(!get32(b,p,viewCount)||viewCount>MAX_VIEWS)return false;
 for(uint32_t view=0;view<viewCount;view++){CachedLibraryView v; uint32_t itemCount;
  if(!gs(b,p,v.id)||!gs(b,p,v.name)||!gs(b,p,v.collectionType)||!get32(b,p,itemCount)||itemCount>MAX_ITEMS)return false;
  for(uint32_t item=0;item<itemCount;item++){MediaItem i;if(!gi(b,p,i))return false;v.items.push_back(std::move(i));}
  vs.push_back(std::move(v));
 } return true;
}
std::string tag(const MediaItem&i){auto p=i.imageTags.find("Primary");return p==i.imageTags.end()?"":p->second;}
}
std::string LibraryCache::scopeKey(const std::string&u,const std::string&id){return catalog::scopeKey(u,id);}
bool LibraryCache::load(const std::string&path,LibrarySnapshot&o,std::string*e,bool *needsRefresh){FILE*f=std::fopen(path.c_str(),"rb");if(!f){if(e)*e="not found";return false;}std::fseek(f,0,SEEK_END);long z=std::ftell(f);std::fseek(f,0,SEEK_SET);if(z<8||z>128*1024*1024){std::fclose(f);if(e)*e="invalid size";return false;}std::vector<unsigned char>b((size_t)z);bool ok=std::fread(b.data(),1,b.size(),f)==b.size();std::fclose(f);size_t p=0;uint32_t v;LibrarySnapshot t;if(!ok||b[0]!='M'||b[1]!='F'||b[2]!='L'||b[3]!='C'||(p=4,!get32(b,p,v))||v<1||v>VERSION||!gv(b,p,t.movies)||!gv(b,p,t.shows)||(v>=2&&(!gm(b,p,t.continueWatching)||!gm(b,p,t.recentlyAdded)))||p!=b.size()){if(e)*e="invalid cache";return false;}if(needsRefresh)*needsRefresh=(v<VERSION);o=std::move(t);return true;}
bool LibraryCache::itemEquivalent(const MediaItem&a,const MediaItem&b){return a.id==b.id&&a.title==b.title&&a.overview==b.overview&&a.year==b.year&&a.rating==b.rating&&a.genre==b.genre&&a.type==b.type&&a.genres==b.genres&&a.played==b.played&&a.progress==b.progress&&a.playbackPositionTicks==b.playbackPositionTicks&&a.imageTags==b.imageTags&&a.indexNumber==b.indexNumber&&a.parentIndexNumber==b.parentIndexNumber&&a.runTimeTicks==b.runTimeTicks&&a.seriesName==b.seriesName&&a.seriesId==b.seriesId&&a.seasonId==b.seasonId;}
ReconcileStats LibraryCache::reconcile(const LibrarySnapshot&o,const LibrarySnapshot&r,std::vector<StalePoster>*st){ReconcileStats x;std::map<std::string,MediaItem> old,rem;auto add=[&](const std::vector<CachedLibraryView>&v,std::map<std::string,MediaItem>&m){for(auto&a:v)for(auto&i:a.items)m[i.id]=i;};add(o.movies,old);add(o.shows,old);add(r.movies,rem);add(r.shows,rem);for(auto&a:rem){auto p=old.find(a.first);if(p==old.end()){x.added++;if(!tag(a.second).empty())x.postersNeeded++;}else{bool ch=(!a.second.etag.empty()&&!p->second.etag.empty())?a.second.etag!=p->second.etag:!itemEquivalent(p->second,a.second);if(ch)x.changed++;else x.unchanged++;if(tag(a.second)!=tag(p->second)){if(!tag(a.second).empty())x.postersNeeded++;if(!tag(p->second).empty()){x.stalePosters++;if(st)st->push_back({p->second.id,tag(p->second)});}}}}for(auto&a:old)if(!rem.count(a.first)){x.removed++;if(!tag(a.second).empty()){x.stalePosters++;if(st)st->push_back({a.second.id,tag(a.second)});}}return x;}
}
