#ifndef MIYOOFIN_MOVIE_TITLE_HPP
#define MIYOOFIN_MOVIE_TITLE_HPP

#include "TitleOrganization.hpp"

namespace miyoofin {

// The title used only to organize Movies; MediaItem::title always remains the
// server-provided display title.
inline std::string_view movieOrganizationalTitle(const std::string &title) { return organizationalTitle(title); }

inline int movieAlphabetIndex(const std::string &title) { return alphabetIndex(title); }

inline int movieAlphabetFocus(const std::string &title) { return alphabetFocus(title); }

inline bool movieMatchesAlphabetFilter(const std::string &title, int letter) { return matchesAlphabetFilter(title,letter); }

inline bool movieOrganizationalLess(const MediaItem &left, const MediaItem &right) { return organizationalLess(left,right); }

} // namespace miyoofin

#endif // MIYOOFIN_MOVIE_TITLE_HPP
