#ifndef MIYOOFIN_SERVER_ADDRESS_HPP
#define MIYOOFIN_SERVER_ADDRESS_HPP

#include <cctype>
#include <string>

namespace miyoofin {

/// True only for explicit IPv4 private/loopback hosts and localhost.
inline bool isObviousLanServerUrl(const std::string &url)
{
    const std::string::size_type scheme=url.find("://");
    if (scheme==std::string::npos || scheme==0) return false;
    const std::string::size_type authorityStart=scheme+3;
    const std::string::size_type authorityEnd=url.find_first_of("/?#",authorityStart);
    const std::string authority=url.substr(authorityStart,authorityEnd-authorityStart);
    if (authority.empty() || authority.find('@')!=std::string::npos) return false;

    std::string host=authority;
    const std::string::size_type colon=authority.find(':');
    if (colon!=std::string::npos) {
        if (authority.find(':',colon+1)!=std::string::npos || colon==0 || colon+1==authority.size()) return false;
        int port=0;
        for (std::string::size_type i=colon+1; i<authority.size(); ++i) {
            if (!std::isdigit(static_cast<unsigned char>(authority[i]))) return false;
            port=port*10+(authority[i]-'0');
            if (port>65535) return false;
        }
        host=authority.substr(0,colon);
    }

    std::string lower;
    lower.reserve(host.size());
    for (char c:host) lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    if (lower=="localhost") return true;

    int octets[4]={};
    std::string::size_type start=0;
    for (int i=0; i<4; ++i) {
        const std::string::size_type end=host.find('.',start);
        if ((i<3 && end==std::string::npos) || (i==3 && end!=std::string::npos)) return false;
        const std::string part=host.substr(start,(end==std::string::npos ? host.size() : end)-start);
        if (part.empty() || part.size()>3) return false;
        int value=0;
        for (char c:part) {
            if (!std::isdigit(static_cast<unsigned char>(c))) return false;
            value=value*10+(c-'0');
        }
        if (value>255) return false;
        octets[i]=value;
        start=(end==std::string::npos ? host.size() : end+1);
    }
    return octets[0]==10 || octets[0]==127 ||
        (octets[0]==172 && octets[1]>=16 && octets[1]<=31) ||
        (octets[0]==192 && octets[1]==168);
}

} // namespace miyoofin

#endif
