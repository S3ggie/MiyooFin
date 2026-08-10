#include "MockData.hpp"

namespace miyoofin {

// -------------------------------------------------------------------
// Helper to create a MediaItem with a deterministic placeholder colour.
// -------------------------------------------------------------------
static MediaItem makeItem(const std::string &id,
                          const std::string &title,
                          const std::string &overview,
                          int year, float rating,
                          const std::string &genre,
                          const std::string &type)
{
    Uint8 r = (Uint8)((title.size() * 37 + 80)  & 0xFF);
    Uint8 g = (Uint8)((title.size() * 53 + 160) & 0xFF);
    Uint8 b = (Uint8)((title.size() * 71 + 240) & 0xFF);
    r = 80 + r % 120;
    g = 80 + g % 120;
    b = 80 + b % 120;

    MediaItem item;
    item.id = id;
    item.title = title;
    item.overview = overview;
    item.year = year;
    item.rating = rating;
    item.genre = genre;
    item.type = type;
    item.artR = r;
    item.artG = g;
    item.artB = b;
    return item;
}
// -------------------------------------------------------------------
// Mock movie rows
// -------------------------------------------------------------------
static std::vector<MediaRow> makeMovieRows()
{
    std::vector<MediaRow> rows;

    rows.push_back({"Action", {
        makeItem("m01", "John Wick",         "A legendary hitman returns from retirement.",                  2014, 4.2f, "Action", "movie"),
        makeItem("m02", "Mad Max: Fury Road", "A post-apocalyptic chase across the wasteland.",              2015, 4.6f, "Action", "movie"),
        makeItem("m03", "Die Hard",          "A cop takes on terrorists in an LA skyscraper.",                1988, 4.4f, "Action", "movie"),
        makeItem("m04", "The Raid",          "An elite squad storms a high-rise crime lord's fortress.",      2011, 4.1f, "Action", "movie"),
        makeItem("m05", "Speed",             "A bomb on a bus -- keep it above 50 mph.",                     1994, 3.9f, "Action", "movie"),
        makeItem("m06", "Gladiator",         "A Roman general seeks vengeance in the arena.",                 2000, 4.5f, "Action", "movie"),
    }});

    rows.push_back({"Sci-Fi", {
        makeItem("m07", "The Matrix",        "A hacker discovers reality is a simulation.",                  1999, 4.7f, "Sci-Fi", "movie"),
        makeItem("m08", "Blade Runner 2049", "A new blade runner unearths a long-buried secret.",             2017, 4.3f, "Sci-Fi", "movie"),
        makeItem("m09", "Interstellar",      "Astronauts travel through a wormhole to save humanity.",       2014, 4.8f, "Sci-Fi", "movie"),
        makeItem("m10", "Dune",              "A noble clan fights for control of a desert planet.",           2021, 4.4f, "Sci-Fi", "movie"),
        makeItem("m11", "Alien",             "A commercial crew encounters a deadly extraterrestrial.",       1979, 4.6f, "Sci-Fi", "movie"),
    }});

    rows.push_back({"Comedy", {
        makeItem("m12", "The Big Lebowski",  "A case of mistaken identity leads to chaos.",                   1998, 4.3f, "Comedy", "movie"),
        makeItem("m13", "Superbad",          "Two high-schoolers try to make the most of prom night.",        2007, 4.0f, "Comedy", "movie"),
        makeItem("m14", "Airplane!",         "An ex-fighter pilot must land a plane with a sick crew.",       1980, 4.2f, "Comedy", "movie"),
        makeItem("m15", "Ghostbusters",      "Paranormal exterminators save New York.",                       1984, 4.1f, "Comedy", "movie"),
        makeItem("m16", "Groundhog Day",     "A weatherman is stuck reliving the same day.",                  1993, 4.4f, "Comedy", "movie"),
    }});

    rows.push_back({"Drama", {
        makeItem("m17", "The Shawshank Redemption", "A banker finds hope in prison.",                        1994, 4.9f, "Drama", "movie"),
        makeItem("m18", "The Godfather",             "The story of a powerful crime dynasty.",               1972, 4.8f, "Drama", "movie"),
        makeItem("m19", "Pulp Fiction",              "Interwoven tales of crime and redemption.",             1994, 4.7f, "Drama", "movie"),
        makeItem("m20", "Goodfellas",                "The rise and fall of a mob associate.",                1990, 4.6f, "Drama", "movie"),
        makeItem("m21", "Fight Club",                "An insomniac and a soapmaker start an underground club.",1999, 4.5f, "Drama", "movie"),
    }});

    return rows;
}
// -------------------------------------------------------------------
// Mock show rows
// -------------------------------------------------------------------
static std::vector<MediaRow> makeShowRows()
{
    std::vector<MediaRow> rows;

    rows.push_back({"Drama", {
        makeItem("s01", "Breaking Bad",      "A chemistry teacher turns to cooking meth.",                   2008, 4.9f, "Drama", "show"),
        makeItem("s02", "The Wire",          "Life on the streets of Baltimore.",                            2002, 4.8f, "Drama", "show"),
        makeItem("s03", "Mad Men",           "The cutthroat world of 1960s advertising.",                    2007, 4.5f, "Drama", "show"),
        makeItem("s04", "Succession",        "A media dynasty fights for control.",                          2018, 4.6f, "Drama", "show"),
        makeItem("s05", "The Crown",         "The reign of Queen Elizabeth II.",                             2016, 4.4f, "Drama", "show"),
    }});

    rows.push_back({"Sci-Fi", {
        makeItem("s06", "Stranger Things",   "Kids uncover supernatural mysteries in a small town.",         2016, 4.6f, "Sci-Fi", "show"),
        makeItem("s07", "Black Mirror",      "Anthology series exploring dark tech futures.",                 2011, 4.4f, "Sci-Fi", "show"),
        makeItem("s08", "The Expanse",       "Humanity colonises the solar system.",                         2015, 4.5f, "Sci-Fi", "show"),
        makeItem("s09", "Westworld",         "A theme park with android hosts goes awry.",                   2016, 4.3f, "Sci-Fi", "show"),
        makeItem("s10", "Dark",              "Time travel unravels the fabric of a small German town.",       2017, 4.5f, "Sci-Fi", "show"),
    }});

    rows.push_back({"Comedy", {
        makeItem("s11", "The Office (US)",   "A mockumentary about office life.",                            2005, 4.5f, "Comedy", "show"),
        makeItem("s12", "Parks and Recreation", "The antics of a small-town parks department.",              2009, 4.4f, "Comedy", "show"),
        makeItem("s13", "Brooklyn Nine-Nine",  "A detective comedy set in a NY precinct.",                   2013, 4.3f, "Comedy", "show"),
        makeItem("s14", "Arrested Development", "A wealthy family loses everything.",                        2003, 4.6f, "Comedy", "show"),
        makeItem("s15", "Community",           "A study group at a community college.",                      2009, 4.4f, "Comedy", "show"),
    }});

    return rows;
}

// -------------------------------------------------------------------
// Singleton: all tabs with their rows
// -------------------------------------------------------------------
const std::vector<TabData>& getMockTabs()
{
    static const std::vector<TabData> tabs = {
        {"Home", {
            {"Continue Watching", {
                makeItem("c01", "The Matrix",        "A hacker discovers reality is a simulation.",      1999, 4.7f, "Sci-Fi", "movie"),
                makeItem("c02", "Breaking Bad",      "A chemistry teacher turns to cooking meth.",        2008, 4.9f, "Drama",  "show"),
                makeItem("c03", "Stranger Things",   "Kids uncover supernatural mysteries.",              2016, 4.6f, "Sci-Fi", "show"),
                makeItem("c04", "Mad Max: Fury Road", "A post-apocalyptic chase across the wasteland.",   2015, 4.6f, "Action", "movie"),
            }},
            {"Trending", {
                makeItem("t01", "Dune",               "A noble clan fights for control of a desert planet.",2021, 4.4f, "Sci-Fi", "movie"),
                makeItem("t02", "Succession",          "A media dynasty fights for control.",               2018, 4.6f, "Drama",  "show"),
                makeItem("t03", "The Crown",          "The reign of Queen Elizabeth II.",                  2016, 4.4f, "Drama",  "show"),
                makeItem("t04", "Fight Club",         "An insomniac and a soapmaker start a club.",        1999, 4.5f, "Drama",  "movie"),
                makeItem("t05", "Gladiator",          "A Roman general seeks vengeance in the arena.",      2000, 4.5f, "Action", "movie"),
            }},
            {"Recently Added", {
                makeItem("r01", "Blade Runner 2049",  "A new blade runner unearths a long-buried secret.", 2017, 4.3f, "Sci-Fi", "movie"),
                makeItem("r02", "Dark",               "Time travel unravels the fabric of a small town.",  2017, 4.5f, "Sci-Fi", "show"),
                makeItem("r03", "The Expanse",        "Humanity colonises the solar system.",              2015, 4.5f, "Sci-Fi", "show"),
                makeItem("r04", "The Office (US)",    "A mockumentary about office life.",                 2005, 4.5f, "Comedy", "show"),
            }},
        }},
        {"Movies", makeMovieRows()},
        {"Shows",  makeShowRows()},
        {"Downloads", {
            {"", {}} // empty -- placeholder
        }},
    };
    return tabs;
}

} // namespace miyoofin
