#ifndef MIYOOFIN_TITLE_ORGANIZATION_HPP
#define MIYOOFIN_TITLE_ORGANIZATION_HPP

#include "../data/MediaItem.hpp"
#include <string>
#include <string_view>

namespace miyoofin {
inline std::string_view organizationalTitle(const std::string &title) {
    if (title.size() > 4 && (title[0]=='T'||title[0]=='t') && (title[1]=='H'||title[1]=='h') &&
        (title[2]=='E'||title[2]=='e') && title[3]==' ') return std::string_view(title).substr(4);
    return title;
}
inline int asciiCaseInsensitiveCompare(std::string_view a, std::string_view b) {
    const size_t n=a.size()<b.size()?a.size():b.size();
    for(size_t i=0;i<n;++i) { unsigned char x=a[i],y=b[i]; if(x>='A'&&x<='Z')x+=32; if(y>='A'&&y<='Z')y+=32; if(x!=y)return x<y?-1:1; }
    return a.size()==b.size()?0:(a.size()<b.size()?-1:1);
}
inline int alphabetIndex(const std::string &title) { std::string_view t=organizationalTitle(title); if(t.empty())return -1; unsigned char c=t[0]; return c>='A'&&c<='Z'?c-'A':c>='a'&&c<='z'?c-'a':-1; }
inline int alphabetFocus(const std::string &title) { int i=alphabetIndex(title); return i<0?0:i; }
inline bool matchesAlphabetFilter(const std::string &title,int letter) { return letter<0||alphabetIndex(title)==letter; }
inline bool organizationalLess(const MediaItem &a,const MediaItem &b) { int c=asciiCaseInsensitiveCompare(organizationalTitle(a.title),organizationalTitle(b.title)); return c?c<0:(a.title!=b.title?a.title<b.title:a.id<b.id); }
}
#endif
