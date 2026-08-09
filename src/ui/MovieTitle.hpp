#ifndef MIYOOFIN_MOVIE_TITLE_HPP
#define MIYOOFIN_MOVIE_TITLE_HPP

#include "../data/MediaItem.hpp"
#include <string>
#include <string_view>

namespace miyoofin {

// The title used only to organize Movies; MediaItem::title always remains the
// server-provided display title.
inline std::string_view movieOrganizationalTitle(const std::string &title)
{
    if (title.size() > 4 &&
        (title[0] == 'T' || title[0] == 't') &&
        (title[1] == 'H' || title[1] == 'h') &&
        (title[2] == 'E' || title[2] == 'e') &&
        title[3] == ' ') {
        return std::string_view(title).substr(4);
    }
    return title;
}

inline int movieAlphabetIndex(const std::string &title)
{
    const std::string_view organizational = movieOrganizationalTitle(title);
    if (organizational.empty()) return -1;
    const unsigned char first = static_cast<unsigned char>(organizational[0]);
    if (first >= 'A' && first <= 'Z') return first - 'A';
    if (first >= 'a' && first <= 'z') return first - 'a';
    return -1;
}

inline int movieAlphabetFocus(const std::string &title)
{
    const int index = movieAlphabetIndex(title);
    return index < 0 ? 0 : index;
}

inline bool movieMatchesAlphabetFilter(const std::string &title, int letter)
{
    return letter < 0 || movieAlphabetIndex(title) == letter;
}

inline int asciiCaseInsensitiveCompare(std::string_view left,
                                        std::string_view right)
{
    const size_t count = left.size() < right.size() ? left.size() : right.size();
    for (size_t i = 0; i < count; ++i) {
        unsigned char a = static_cast<unsigned char>(left[i]);
        unsigned char b = static_cast<unsigned char>(right[i]);
        if (a >= 'A' && a <= 'Z') a = static_cast<unsigned char>(a + ('a' - 'A'));
        if (b >= 'A' && b <= 'Z') b = static_cast<unsigned char>(b + ('a' - 'A'));
        if (a != b) return a < b ? -1 : 1;
    }
    if (left.size() == right.size()) return 0;
    return left.size() < right.size() ? -1 : 1;
}

inline bool movieOrganizationalLess(const MediaItem &left, const MediaItem &right)
{
    const int organizational = asciiCaseInsensitiveCompare(
        movieOrganizationalTitle(left.title), movieOrganizationalTitle(right.title));
    if (organizational != 0) return organizational < 0;
    if (left.title != right.title) return left.title < right.title;
    return left.id < right.id;
}

} // namespace miyoofin

#endif // MIYOOFIN_MOVIE_TITLE_HPP
