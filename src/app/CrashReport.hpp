#ifndef MIYOOFIN_CRASH_REPORT_HPP
#define MIYOOFIN_CRASH_REPORT_HPP

#include <cstdlib>
#include <ctime>
#include <string>
#include <vector>

namespace miyoofin {

/// What crash.log (see CrashLog.cpp) says about its newest crash, as short display lines.
struct CrashReport
{
    int crashes = 0;
    std::vector<std::string> lines; // newest crash: signal, time, pc/lr, a few stack frames
};

inline const char* crashSignalName(int sig)
{
    switch (sig) {
    case 4:
        return "illegal instruction";
    case 6:
        return "abort";
    case 7:
        return "bus error";
    case 8:
        return "arithmetic error";
    case 11:
        return "segmentation fault";
    default:
        return "fatal signal";
    }
}

inline CrashReport summarizeCrashLog(const std::string& text, std::size_t maxFrames = 8)
{
    static const std::string kHead = "--- fatal signal ";
    CrashReport report;
    std::size_t last = std::string::npos, prev = std::string::npos;
    for (std::size_t at = text.find(kHead); at != std::string::npos;
         at = text.find(kHead, at + 1)) {
        prev = last;
        last = at;
        ++report.crashes;
    }
    if (last == std::string::npos)
        return report;
    const int sig = std::atoi(text.c_str() + last + kHead.size());
    const std::size_t atPos = text.find(" at t=", last);
    const long when = atPos == std::string::npos ? 0 : std::atol(text.c_str() + atPos + 6);
    report.lines.push_back("Signal " + std::to_string(sig) + " (" + crashSignalName(sig) + ")");
    if (when > 0) {
        char buf[48];
        const time_t t = static_cast<time_t>(when);
        std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", std::localtime(&t));
        report.lines.push_back(std::string("At ") + buf);
    }
    // "pc=... lr=... addr=..." is written just before the header of the same crash.
    const std::size_t pc = text.rfind("pc=", last);
    if (pc != std::string::npos && (prev == std::string::npos || pc > prev)) {
        const std::size_t eol = text.find('\n', pc);
        report.lines.push_back(text.substr(pc, eol == std::string::npos ? eol : eol - pc));
    }
    // The backtrace follows the header, up to a blank line.
    std::size_t pos = text.find('\n', last);
    std::size_t frames = 0;
    while (pos != std::string::npos && pos + 1 < text.size() && frames < maxFrames) {
        const std::size_t eol = text.find('\n', pos + 1);
        const std::string line =
            text.substr(pos + 1, eol == std::string::npos ? eol : eol - pos - 1);
        if (line.empty() || line.compare(0, 3, "pc=") == 0)
            break;
        report.lines.push_back(line);
        ++frames;
        pos = eol;
    }
    return report;
}

} // namespace miyoofin

#endif
