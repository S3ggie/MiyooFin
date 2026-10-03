#include "test_support.hpp"

#include "../src/app/CrashReport.hpp"

using namespace miyoofin;

int main()
{
    CHECK(summarizeCrashLog("").crashes == 0);
    CHECK(summarizeCrashLog("random text\n").lines.empty());

    const std::string log =
        "pc=0x1111 lr=0x2222 addr=0x0\nstack: 0x1 0x2\n--- fatal signal 6 at t=1700000000\n"
        "./app(+0x10)[0x10]\n\n"
        "pc=0xdead lr=0xbeef addr=0x8\nstack: 0x3\n--- fatal signal 11 at t=1700003600\n"
        "./app(+0x20)[0x20]\n./app(+0x30)[0x30]\n\n";
    const CrashReport r = summarizeCrashLog(log);
    CHECK(r.crashes == 2);
    CHECK_EQ(r.lines[0], "Signal 11 (segmentation fault)");
    bool pcFound = false, frameFound = false, oldFound = false;
    for (const std::string& line : r.lines) {
        pcFound |= line == "pc=0xdead lr=0xbeef addr=0x8";
        frameFound |= line == "./app(+0x30)[0x30]";
        oldFound |= line.find("0x1111") != std::string::npos || line == "./app(+0x10)[0x10]";
    }
    CHECK(pcFound && frameFound && !oldFound);

    // Only the newest crash's pc line counts, even when it lacks one.
    const CrashReport noPc =
        summarizeCrashLog("pc=0x1 lr=0x2 addr=0x0\n--- fatal signal 6 at t=1700000000\nf1\n\n"
                          "--- fatal signal 7 at t=1700000100\nf2\n\n");
    CHECK(noPc.crashes == 2);
    for (const std::string& line : noPc.lines)
        CHECK(line.compare(0, 3, "pc=") != 0);
    return miyoofin_test::finish("crash_report");
}
