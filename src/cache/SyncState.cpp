#include "SyncState.hpp"
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>
namespace miyoofin { namespace {
bool dirs(const std::string&p){for(size_t i=1;i<=p.size();++i)if(i==p.size()||p[i]=='/'){std::string d=p.substr(0,i);if(!d.empty()&&mkdir(d.c_str(),0755)&&errno!=EEXIST)return false;}return true;}
}
std::string SyncStateStore::path(const std::string&r,const std::string&s){return r+"/library/"+s+"/sync-state.v1";}
bool SyncStateStore::save(const std::string&p,const SyncState&s,std::string*e){auto x=p.find_last_of('/');if(x!=std::string::npos&&!dirs(p.substr(0,x))){if(e)*e="mkdir failed";return false;}std::string t=p+".tmp."+std::to_string((long long)getpid());FILE*f=fopen(t.c_str(),"wb");bool ok=f&&fprintf(f,"MFS1 %lld %lld\n",(long long)s.lastSuccessfulMs,(long long)s.lastReconcileMs)>0&&fflush(f)==0&&fsync(fileno(f))==0&&fclose(f)==0;if(!ok||rename(t.c_str(),p.c_str())){if(f)fclose(f);remove(t.c_str());if(e)*e="write failed";return false;}return true;}
bool SyncStateStore::load(const std::string&p,SyncState&o,std::string*e){FILE*f=fopen(p.c_str(),"rb");if(!f){if(e)*e="not found";return false;}char magic[5]={};long long successful=0,reconcile=0;bool ok=fscanf(f,"%4s %lld %lld",magic,&successful,&reconcile)==3&&!strcmp(magic,"MFS1")&&successful>=0&&reconcile>=0&&fgetc(f)=='\n'&&fgetc(f)==EOF;fclose(f);if(!ok){if(e)*e="invalid sync state";return false;}o.lastSuccessfulMs=successful;o.lastReconcileMs=reconcile;return true;}
}
