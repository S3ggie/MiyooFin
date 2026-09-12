#ifndef MIYOOFIN_TITLE_ORGANIZATION_HPP
#define MIYOOFIN_TITLE_ORGANIZATION_HPP

#include "../data/MediaItem.hpp"
#include "../catalog/CatalogPrimitives.hpp"
#include <string>
#include <string_view>
#include <string>

namespace miyoofin {
inline std::string_view organizationalTitle(const std::string &title) { return catalog::organizationalTitle(title); }
inline std::string organizationalSortKey(const std::string &title) { return catalog::organizationalSortKey(title); }
inline int asciiCaseInsensitiveCompare(std::string_view a, std::string_view b) { return catalog::asciiCaseInsensitiveCompare(a,b); }
inline int alphabetIndex(const std::string &title) { return catalog::alphabetIndex(title); }
inline int alphabetFocus(const std::string &title) { int i=alphabetIndex(title); return i<0?0:i; }
inline bool matchesAlphabetFilter(const std::string &title,int letter) { return letter<0||alphabetIndex(title)==letter; }
inline bool organizationalLess(const MediaItem &a,const MediaItem &b) { int c=asciiCaseInsensitiveCompare(organizationalTitle(a.title),organizationalTitle(b.title)); return c?c<0:(a.title!=b.title?a.title<b.title:a.id<b.id); }
}
#endif
