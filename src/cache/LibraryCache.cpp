#include "LibraryCache.hpp"
#include "../net/JellyfinApi.hpp"
#include <cstdio>
#include <cstdint>
#include <cerrno>
#include <sys/stat.h>
#include <unistd.h>
#include <map>
#include <cstring>

namespace miyoofin {
namespace {
constexpr uint32_t VERSION=2, MAX_STRING=1024*1024, MAX_ITEMS=100000, MAX_VIEWS=1024;
void put32(std::vector<unsigned char>& b,uint32_t n){for(int i=0;i<4;i++)b.push_back((unsigned char)(n>>(i*8)));}
void put64(std::vector<unsigned char>& b,uint64_t n){for(int i=0;i<8;i++)b.push_back((unsigned char)(n>>(i*8)));}
bool get32(const std::vector<unsigned char>&b,size_t&p,uint32_t&n){if(p+4>b.size())return false;n=0;for(int i=0;i<4;i++)n|=(uint32_t)b[p++]<<(i*8);return true;}
bool get64(const std::vector<unsigned char>&b,size_t&p,uint64_t&n){if(p+8>b.size())return false;n=0;for(int i=0;i<8;i++)n|=(uint64_t)b[p++]<<(i*8);return true;}
void ps(std::vector<unsigned char>&b,const std::string&s){put32(b,(uint32_t)s.size());b.insert(b.end(),s.begin(),s.end());}
bool gs(const std::vector<unsigned char>&b,size_t&p,std::string&s){uint32_t n;if(!get32(b,p,n)||n>MAX_STRING||p+n>b.size())return false;s.assign((const char*)&b[p],n);p+=n;return true;}
void pi(std::vector<unsigned char>&b,const MediaItem&i){ps(b,i.id);ps(b,i.title);ps(b,i.overview);put32(b,(uint32_t)i.year);uint32_t r;std::memcpy(&r,&i.rating,4);put32(b,r);ps(b,i.genre);ps(b,i.type);ps(b,i.etag);put32(b,i.played);std::memcpy(&r,&i.progress,4);put32(b,r);put64(b,(uint64_t)i.playbackPositionTicks);put32(b,(uint32_t)i.genres.size());for(auto&s:i.genres)ps(b,s);put32(b,(uint32_t)i.imageTags.size());for(auto&x:i.imageTags){ps(b,x.first);ps(b,x.second);}put32(b,i.indexNumber);put32(b,i.parentIndexNumber);put64(b,(uint64_t)i.runTimeTicks);ps(b,i.seriesName);ps(b,i.seriesId);ps(b,i.seasonId);b.push_back(i.artR);b.push_back(i.artG);b.push_back(i.artB);}
bool gi(const std::vector<unsigned char>&b,size_t&p,MediaItem&i){uint32_t n,r;uint64_t q;if(!gs(b,p,i.id)||!gs(b,p,i.title)||!gs(b,p,i.overview)||!get32(b,p,n)||!get32(b,p,r)||!gs(b,p,i.genre)||!gs(b,p,i.type)||!gs(b,p,i.etag))return false;i.year=(int)n;std::memcpy(&i.rating,&r,4);if(!get32(b,p,n)||!get32(b,p,r)||!get64(b,p,q))return false;i.played=n!=0;std::memcpy(&i.progress,&r,4);i.playbackPositionTicks=(long long)q;if(!get32(b,p,n)||n>MAX_ITEMS)return false;for(uint32_t k=0;k<n;k++){std::string s;if(!gs(b,p,s))return false;i.genres.push_back(s);}if(!get32(b,p,n)||n>MAX_ITEMS)return false;for(uint32_t k=0;k<n;k++){std::string a,c;if(!gs(b,p,a)||!gs(b,p,c))return false;i.imageTags[a]=c;}if(!get32(b,p,n))return false;i.indexNumber=(int)n;if(!get32(b,p,n)||!get64(b,p,q)||!gs(b,p,i.seriesName)||!gs(b,p,i.seriesId)||!gs(b,p,i.seasonId)||p+3>b.size())return false;i.parentIndexNumber=(int)n;i.runTimeTicks=(long long)q;i.artR=b[p++];i.artG=b[p++];i.artB=b[p++];return true;}
void pv(std::vector<unsigned char>&b,const std::vector<CachedLibraryView>&vs){put32(b,(uint32_t)vs.size());for(auto&v:vs){ps(b,v.id);ps(b,v.name);ps(b,v.collectionType);put32(b,(uint32_t)v.items.size());for(auto&i:v.items)pi(b,i);}}
void pm(std::vector<unsigned char>&b,const std::vector<MediaItem>&vs){put32(b,(uint32_t)vs.size());for(auto&i:vs)pi(b,i);}
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
bool mkdirs(const std::string&p){for(size_t i=1;i<=p.size();++i)if(i==p.size()||p[i]=='/') {auto d=p.substr(0,i);if(!d.empty()&&::mkdir(d.c_str(),0755)&&errno!=EEXIST)return false;}return true;}
}
std::string LibraryCache::scopeKey(const std::string&u,const std::string&id){std::string s=JellyfinApi::normaliseUrl(u)+"\n"+id;uint64_t h=1469598103934665603ULL;for(unsigned char c:s){h^=c;h*=1099511628211ULL;}char x[17];std::snprintf(x,sizeof x,"%016llx",(unsigned long long)h);return x;}
std::string LibraryCache::cachePath(const std::string&r,const std::string&s){return r+"/library/"+s+"/snapshot.v1";}
bool LibraryCache::save(const std::string&path,const LibrarySnapshot&s,std::string*e){auto slash=path.find_last_of('/');if(slash!=std::string::npos&&!mkdirs(path.substr(0,slash))){if(e)*e="mkdir failed";return false;}std::vector<unsigned char>b={'M','F','L','C'};put32(b,VERSION);pv(b,s.movies);pv(b,s.shows);pm(b,s.continueWatching);pm(b,s.recentlyAdded);std::string tmp=path+".tmp";FILE*f=std::fopen(tmp.c_str(),"wb");if(!f){if(e)*e="open failed";return false;}bool ok=std::fwrite(b.data(),1,b.size(),f)==b.size(); if(std::fclose(f)!=0)ok=false; if(!ok||std::rename(tmp.c_str(),path.c_str())!=0){std::remove(tmp.c_str());if(e)*e="write/rename failed";return false;}return true;}
bool LibraryCache::load(const std::string&path,LibrarySnapshot&o,std::string*e){FILE*f=std::fopen(path.c_str(),"rb");if(!f){if(e)*e="not found";return false;}std::fseek(f,0,SEEK_END);long z=std::ftell(f);std::fseek(f,0,SEEK_SET);if(z<8||z>128*1024*1024){std::fclose(f);if(e)*e="invalid size";return false;}std::vector<unsigned char>b((size_t)z);bool ok=std::fread(b.data(),1,b.size(),f)==b.size();std::fclose(f);size_t p=0;uint32_t v;LibrarySnapshot t;if(!ok||b[0]!='M'||b[1]!='F'||b[2]!='L'||b[3]!='C'||(p=4,!get32(b,p,v))||(v!=1&&v!=VERSION)||!gv(b,p,t.movies)||!gv(b,p,t.shows)||(v>=2&&(!gm(b,p,t.continueWatching)||!gm(b,p,t.recentlyAdded)))||p!=b.size()){if(e)*e="invalid cache";return false;}o=std::move(t);return true;}
bool LibraryCache::itemEquivalent(const MediaItem&a,const MediaItem&b){return a.id==b.id&&a.title==b.title&&a.overview==b.overview&&a.year==b.year&&a.rating==b.rating&&a.genre==b.genre&&a.type==b.type&&a.genres==b.genres&&a.played==b.played&&a.progress==b.progress&&a.playbackPositionTicks==b.playbackPositionTicks&&a.imageTags==b.imageTags&&a.indexNumber==b.indexNumber&&a.parentIndexNumber==b.parentIndexNumber&&a.runTimeTicks==b.runTimeTicks&&a.seriesName==b.seriesName&&a.seriesId==b.seriesId&&a.seasonId==b.seasonId;}
ReconcileStats LibraryCache::reconcile(const LibrarySnapshot&o,const LibrarySnapshot&r,std::vector<StalePoster>*st){ReconcileStats x;std::map<std::string,MediaItem> old,rem;auto add=[&](const std::vector<CachedLibraryView>&v,std::map<std::string,MediaItem>&m){for(auto&a:v)for(auto&i:a.items)m[i.id]=i;};add(o.movies,old);add(o.shows,old);add(r.movies,rem);add(r.shows,rem);for(auto&a:rem){auto p=old.find(a.first);if(p==old.end()){x.added++;if(!tag(a.second).empty())x.postersNeeded++;}else{bool ch=(!a.second.etag.empty()&&!p->second.etag.empty())?a.second.etag!=p->second.etag:!itemEquivalent(p->second,a.second);if(ch)x.changed++;else x.unchanged++;if(tag(a.second)!=tag(p->second)){if(!tag(a.second).empty())x.postersNeeded++;if(!tag(p->second).empty()){x.stalePosters++;if(st)st->push_back({p->second.id,tag(p->second)});}}}}for(auto&a:old)if(!rem.count(a.first)){x.removed++;if(!tag(a.second).empty()){x.stalePosters++;if(st)st->push_back({a.second.id,tag(a.second)});}}return x;}
}
