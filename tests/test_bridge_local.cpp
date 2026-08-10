// Socket-level local bridge integration test.  Its fixture is deliberately
// produced by DownloadStore, so the bridge consumes the shipping manifest
// format rather than a hand-written approximation.
#include "../src/download/DownloadStore.hpp"
#include "../src/cache/LibraryCache.hpp"
#include <arpa/inet.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace miyoofin { std::string LibraryCache::scopeKey(const std::string&,const std::string&){ return "unused"; } }
static int fails=0;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL %d: %s\n",__LINE__,#x); ++fails; } } while(0)
static std::string request(int port,const std::string &text) {
    int fd=socket(AF_INET,SOCK_STREAM,0); sockaddr_in a={}; a.sin_family=AF_INET; a.sin_port=htons(port); a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    if (connect(fd,(sockaddr*)&a,sizeof a)) { close(fd); return {}; }
    send(fd,text.data(),text.size(),0); std::string out; char b[256]; ssize_t n; while((n=recv(fd,b,sizeof b,0))>0)out.append(b,n); close(fd); return out;
}
static std::string body(const std::string &response) { size_t p=response.find("\r\n\r\n"); return p==std::string::npos?"":response.substr(p+4); }
static bool has(const std::string&s,const std::string&part){ return s.find(part)!=std::string::npos; }
int main(){
    using namespace miyoofin; const int port=24000+(getpid()%10000); std::string root="/tmp/miyoofin-bridge-local-"+std::to_string((long long)getpid());
    DownloadStore store(root); DownloadItem item; item.itemId="fixture"; item.expectedSize=11; item.chunkSize=4; item.downloadedBytes=11; item.state=DownloadState::Complete;
    CHECK(store.saveManifest("scope",item)); std::string chunks=store.itemPath("scope",item.itemId)+"/chunks"; CHECK(mkdir(chunks.c_str(),0755)==0);
    const char *parts[]={"ABCD","EFGH","IJK"}; for(int i=0;i<3;i++){ FILE*f=fopen(store.chunkPath("scope",item.itemId,i).c_str(),"wb"); CHECK(f); if(f){CHECK(fwrite(parts[i],1,strlen(parts[i]),f)==strlen(parts[i])); fclose(f);} }
    pid_t child=fork(); CHECK(child>=0); if(child==0){ execl("output/build/miyoofin-https-bridge","output/build/miyoofin-https-bridge","--local-manifest",store.manifestPath("scope",item.itemId).c_str(),std::to_string(port).c_str(),(char*)nullptr); _exit(127); }
    std::string ready; for(int i=0;i<50&&ready.empty();++i){ usleep(20000); ready=request(port,"HEAD /stream HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n"); } CHECK(!ready.empty());
    CHECK(has(ready,"HTTP/1.1 200 OK")&&has(ready,"Content-Length: 11")&&body(ready).empty());
    std::string full=request(port,"GET /stream HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n"); CHECK(has(full,"HTTP/1.1 200 OK")&&body(full)=="ABCDEFGHIJK");
    std::string inside=request(port,"GET /stream HTTP/1.1\r\nRange: bytes=5-6\r\n\r\n"); CHECK(has(inside,"206 Partial Content")&&has(inside,"Content-Range: bytes 5-6/11")&&body(inside)=="FG");
    std::string crossing=request(port,"GET /stream HTTP/1.1\r\nRange: bytes=2-5\r\n\r\n"); CHECK(has(crossing,"206 Partial Content")&&body(crossing)=="CDEF");
    std::string final=request(port,"GET /stream HTTP/1.1\r\nRange: bytes=8-\r\n\r\n"); CHECK(has(final,"Content-Range: bytes 8-10/11")&&body(final)=="IJK");
    std::string invalid=request(port,"GET /stream HTTP/1.1\r\nRange: bytes=11-\r\n\r\n"); CHECK(has(invalid,"416 Range Not Satisfiable")&&has(invalid,"Content-Range: bytes */11")&&body(invalid).empty());
    kill(child,SIGTERM); int status=0; waitpid(child,&status,0); CHECK(WIFEXITED(status));

    DownloadItem hls; hls.itemId="hls"; hls.hlsStorage=true; hls.hlsSegmentCount=3; hls.chunkSize=1; hls.state=DownloadState::Complete;
    CHECK(store.saveManifest("scope",hls)); CHECK(store.ensureHlsDirectories("scope",hls.itemId));
    const char *segments[]={"ONE","TW"}; for(int i=0;i<2;i++){ FILE*f=fopen(store.segmentPath("scope",hls.itemId,i).c_str(),"wb"); CHECK(f); if(f){CHECK(fwrite(segments[i],1,strlen(segments[i]),f)==strlen(segments[i])); fclose(f);} }
    child=fork(); CHECK(child>=0); if(child==0){ execl("output/build/miyoofin-https-bridge","output/build/miyoofin-https-bridge","--local-manifest",store.manifestPath("scope",hls.itemId).c_str(),std::to_string(port).c_str(),(char*)nullptr); _exit(127); }
    ready.clear(); for(int i=0;i<50&&ready.empty();++i){ usleep(20000); ready=request(port,"HEAD /local.m3u8 HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n"); } CHECK(has(ready,"HTTP/1.1 200 OK")&&has(ready,"Content-Length:"));
    std::string playlist=request(port,"GET /local.m3u8 HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n"); CHECK(has(playlist,"HTTP/1.1 200 OK")&&has(body(playlist),"#EXTM3U\n")&&has(body(playlist),"/segments/000000\n")&&has(body(playlist),"/segments/000001\n")&&has(body(playlist),"/segments/000002\n"));
    std::string first=request(port,"GET /segments/000000 HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n"); CHECK(has(first,"HTTP/1.1 200 OK")&&has(first,"Content-Length: 3")&&body(first)=="ONE");
    std::string second=request(port,"GET /segments/000001 HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n"); CHECK(has(second,"HTTP/1.1 200 OK")&&has(second,"Content-Length: 2")&&body(second)=="TW");
    std::string missing=request(port,"GET /segments/000002 HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n"); CHECK(has(missing,"HTTP/1.1 404 Not Found")&&body(missing).empty());
    std::string traversal=request(port,"GET /segments/000000/../manifest.v2 HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n"); CHECK(has(traversal,"HTTP/1.1 404 Not Found")&&body(traversal).empty());
    std::string invalidSegment=request(port,"GET /segments/000003 HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n"); CHECK(has(invalidSegment,"HTTP/1.1 404 Not Found")&&body(invalidSegment).empty());
    kill(child,SIGTERM); waitpid(child,&status,0); CHECK(WIFEXITED(status));
    unlink(store.segmentPath("scope",hls.itemId,0).c_str()); unlink(store.segmentPath("scope",hls.itemId,1).c_str()); unlink(store.manifestPath("scope",hls.itemId).c_str()); rmdir((store.itemPath("scope",hls.itemId)+"/segments").c_str()); rmdir(store.itemPath("scope",hls.itemId).c_str());
    unlink(store.chunkPath("scope",item.itemId,0).c_str()); unlink(store.chunkPath("scope",item.itemId,1).c_str()); unlink(store.chunkPath("scope",item.itemId,2).c_str());
    rmdir(chunks.c_str()); unlink(store.manifestPath("scope",item.itemId).c_str()); rmdir(store.itemPath("scope",item.itemId).c_str());
    rmdir((store.scopePath("scope")+"/items").c_str()); rmdir(store.scopePath("scope").c_str()); rmdir(root.c_str());
    if(!fails) std::printf("[test] local bridge integration OK\n"); return fails?1:0;
}
