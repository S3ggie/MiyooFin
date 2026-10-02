#include "RemoteControl.hpp"

#include <cstdio>
#include <cstring>
#include <sys/stat.h>

namespace miyoofin {

namespace {
constexpr const char* kEnableFile = ".remote-control";
constexpr const char* kCommandFile = "/tmp/miyoofin-app-cmd";
constexpr std::size_t kMaxActionsPerPoll = 16;

bool nameToAction(const std::string& name, Action& out)
{
    static const struct
    {
        const char* name;
        Action action;
    } kTable[] = {{"Up", Action::Up},
                  {"Down", Action::Down},
                  {"Left", Action::Left},
                  {"Right", Action::Right},
                  {"Confirm", Action::Confirm},
                  {"Back", Action::Back},
                  {"Search", Action::Search},
                  {"ActionsMenu", Action::ActionsMenu},
                  {"PrevTab", Action::PrevTab},
                  {"NextTab", Action::NextTab},
                  {"PrevPage", Action::PrevPage},
                  {"NextPage", Action::NextPage},
                  {"Settings", Action::Settings},
                  {"Menu", Action::Menu}};
    for (const auto& entry : kTable)
        if (name == entry.name) {
            out = entry.action;
            return true;
        }
    return false;
}
} // namespace

bool remoteControlEnabled()
{
    struct stat st;
    return ::stat(kEnableFile, &st) == 0;
}

void parseRemoteCommands(const std::string& text, std::vector<Action>& out)
{
    std::size_t pos = 0;
    while (pos < text.size() && out.size() < kMaxActionsPerPoll) {
        std::size_t end = text.find('\n', pos);
        if (end == std::string::npos)
            end = text.size();
        std::string line = text.substr(pos, end - pos);
        pos = end + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            line.pop_back();
        if (line == "quit") {
            out.push_back(Action::Exit);
        } else if (line.compare(0, 4, "key ") == 0) {
            Action a;
            if (nameToAction(line.substr(4), a))
                out.push_back(a);
        }
    }
}

void pollRemoteControl(std::vector<Action>& out)
{
    struct stat st;
    if (::stat(kCommandFile, &st) != 0 || !remoteControlEnabled())
        return;
    std::string text;
    if (FILE* f = std::fopen(kCommandFile, "rb")) {
        char buf[512];
        std::size_t n = std::fread(buf, 1, sizeof(buf), f);
        text.assign(buf, n);
        std::fclose(f);
    }
    std::remove(kCommandFile);
    parseRemoteCommands(text, out);
}

} // namespace miyoofin
