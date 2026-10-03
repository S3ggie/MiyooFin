#include "CrashReportScreen.hpp"
#include "../Theme.hpp"
#include "../UiKit.hpp"
#include "../../app/ScreenStack.hpp"
#include <cstdio>
#include <fstream>
#include <sstream>

namespace miyoofin {

namespace {
constexpr const char* kCrashLog = "crash.log";
}

CrashReportScreen::CrashReportScreen()
{
    std::ifstream in(kCrashLog);
    std::stringstream text;
    text << in.rdbuf();
    m_report = summarizeCrashLog(text.str());
}

bool CrashReportScreen::handleAction(Action action)
{
    if (action == Action::Search) { // X
        std::remove(kCrashLog);
        m_report = {};
        return true;
    }
    if (action == Action::Back) {
        if (m_stack)
            m_stack->pop();
        return true;
    }
    return false;
}

void CrashReportScreen::render(SDL_Surface* fb)
{
    namespace d = design;
    ui::fill(fb, 0, 0, d::kScreenW, d::kScreenH, d::kCanvas);
    ui::HeaderSpec header;
    header.title = "Crash report";
    header.showStatus = false;
    ui::header(fb, header);
    int y = 58;
    if (m_report.crashes == 0) {
        ui::text(fb, d::kMargin, y, "No crashes recorded.", d::kTextSecondary);
    } else {
        ui::text(fb, d::kMargin, y,
                 std::to_string(m_report.crashes) + (m_report.crashes == 1 ? " crash" : " crashes") +
                     " recorded - the newest:",
                 d::kAccentHi);
        y += 28;
        for (const std::string& line : m_report.lines) {
            ui::textClamped(fb, d::kMargin, y, d::kScreenW - 2 * d::kMargin, line,
                            y < 100 ? d::kText : d::kTextSecondary);
            y += 22;
        }
    }
    ui::FooterSpec footer;
    footer.showLink = false;
    footer.hints = {{ui::Key::X, "Clear log"}, {ui::Key::B, "Back"}};
    ui::footer(fb, footer);
}

} // namespace miyoofin
