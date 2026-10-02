#include "MusicUiState.hpp"
#include <cstdlib>
#include <sstream>

namespace miyoofin {

namespace {

constexpr const char* kMagic = "MFMU=1";
constexpr int kMaxDrill = 12; // pages deep we are willing to restore

std::string clean(const std::string& s)
{
    std::string out = s;
    for (char& c : out)
        if (c == '\t' || c == '\n' || c == '\r')
            c = ' ';
    return out;
}

std::vector<std::string> split(const std::string& line)
{
    std::vector<std::string> out;
    std::size_t start = 0;
    for (;;) {
        const std::size_t tab = line.find('\t', start);
        if (tab == std::string::npos) {
            out.push_back(line.substr(start));
            return out;
        }
        out.push_back(line.substr(start, tab - start));
        start = tab + 1;
    }
}

void writeFrame(std::ostringstream& out, const char* tag, int tab, const MusicFrame& f)
{
    out << tag << '\t' << tab << '\t' << static_cast<int>(f.kind) << '\t' << clean(f.id) << '\t'
        << clean(f.title) << '\t' << clean(f.subtitle) << '\t' << clean(f.artId) << '\t'
        << clean(f.artTag) << '\t' << clean(f.selectedId) << '\t' << f.selected << '\t' << f.scroll
        << '\t' << (f.letter ? f.letter : '-') << '\n';
}

bool readFrame(const std::vector<std::string>& f, MusicFrame& out)
{
    if (f.size() < 12)
        return false;
    const int kind = std::atoi(f[2].c_str());
    if (kind < 0 || kind > static_cast<int>(MusicPaneKind::PlaylistTracks))
        return false;
    out.kind = static_cast<MusicPaneKind>(kind);
    out.id = f[3];
    out.title = f[4];
    out.subtitle = f[5];
    out.artId = f[6];
    out.artTag = f[7];
    out.selectedId = f[8];
    out.selected = std::max(0, std::atoi(f[9].c_str()));
    out.scroll = std::max(0, std::atoi(f[10].c_str()));
    out.letter = f[11].empty() || f[11][0] == '-' ? 0 : f[11][0];
    return true;
}

MusicFrame root(MusicPaneKind kind)
{
    MusicFrame f;
    f.kind = kind;
    return f;
}

} // namespace

MusicUiState MusicUiState::defaults()
{
    MusicUiState s;
    s.tabs[0].roots = {root(MusicPaneKind::Home)};
    s.tabs[1].roots = {root(MusicPaneKind::Artists), root(MusicPaneKind::Albums),
                       root(MusicPaneKind::Songs)};
    s.tabs[2].roots = {root(MusicPaneKind::Playlists)};
    s.tabs[3].roots = {root(MusicPaneKind::Downloads)};
    s.tabs[4].roots = {root(MusicPaneKind::Settings)};
    return s;
}

std::string MusicUiState::serialize() const
{
    std::ostringstream out;
    out << kMagic << '\n' << "tab\t" << activeTab << '\n';
    for (int t = 0; t < kMusicTabCount; ++t) {
        out << "section\t" << t << '\t' << tabs[t].section << '\n';
        for (const MusicFrame& f : tabs[t].roots)
            writeFrame(out, "root", t, f);
        for (const MusicFrame& f : tabs[t].drill)
            writeFrame(out, "drill", t, f);
    }
    return out.str();
}

bool MusicUiState::parse(const std::string& text, MusicUiState& out)
{
    MusicUiState parsed = defaults();
    std::istringstream in(text);
    std::string line;
    if (!std::getline(in, line) || line != kMagic)
        return false;
    std::array<std::size_t, kMusicTabCount> rootsSeen{};
    while (std::getline(in, line)) {
        if (line.empty())
            continue;
        const auto f = split(line);
        if (f[0] == "tab" && f.size() >= 2) {
            parsed.activeTab = std::atoi(f[1].c_str());
        } else if (f[0] == "section" && f.size() >= 3) {
            const int t = std::atoi(f[1].c_str());
            if (t >= 0 && t < kMusicTabCount)
                parsed.tabs[t].section = std::atoi(f[2].c_str());
        } else if ((f[0] == "root" || f[0] == "drill") && f.size() >= 3) {
            const int t = std::atoi(f[1].c_str());
            MusicFrame frame;
            if (t < 0 || t >= kMusicTabCount || !readFrame(f, frame))
                return false;
            if (f[0] == "root") {
                // Roots keep their fixed kinds; only the cursor fields are restored.
                std::size_t& i = rootsSeen[t];
                if (i < parsed.tabs[t].roots.size() && parsed.tabs[t].roots[i].kind == frame.kind)
                    parsed.tabs[t].roots[i] = frame;
                ++i;
            } else if (static_cast<int>(parsed.tabs[t].drill.size()) < kMaxDrill &&
                       frame.kind >= MusicPaneKind::ArtistAlbums && !frame.id.empty()) {
                parsed.tabs[t].drill.push_back(frame);
            }
        }
    }
    // Clamp what a hand-edited or old file could get wrong.
    if (parsed.activeTab < 0 || parsed.activeTab >= kMusicTabCount)
        parsed.activeTab = 0;
    for (MusicTabState& tab : parsed.tabs)
        if (tab.section < 0 || tab.section >= static_cast<int>(tab.roots.size()))
            tab.section = 0;
    out = parsed;
    return true;
}

} // namespace miyoofin
