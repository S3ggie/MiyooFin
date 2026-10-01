#include "BatteryMonitor.hpp"

#include <cstdio>
#include <utility>

namespace miyoofin {

void BatteryMonitor::update(unsigned dtMs)
{
    if (m_hasRead) {
        m_sinceReadMs += dtMs;
        if (m_sinceReadMs < kRefreshMs)
            return;
    }
    m_hasRead = true;
    m_sinceReadMs = 0;
    m_percent = -1;
    std::FILE* file = std::fopen(m_path.c_str(), "r");
    if (!file)
        return;
    char buffer[16] = {};
    const std::size_t n = std::fread(buffer, 1, sizeof(buffer) - 1, file);
    std::fclose(file);
    m_percent = parsePercent(std::string(buffer, n));
}

int BatteryMonitor::parsePercent(const std::string& content)
{
    std::size_t i = 0;
    while (i < content.size() &&
           (content[i] == ' ' || content[i] == '\t' || content[i] == '\n' || content[i] == '\r'))
        ++i;
    int value = 0;
    std::size_t digits = 0;
    while (i < content.size() && content[i] >= '0' && content[i] <= '9' && digits < 4) {
        value = value * 10 + (content[i] - '0');
        ++i;
        ++digits;
    }
    if (digits == 0 || value > 100)
        return -1;
    // Only trailing whitespace may follow the number.
    while (i < content.size()) {
        const char c = content[i++];
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r' && c != '\0')
            return -1;
    }
    return value;
}

int BatteryMonitor::fillWidth(int percent, int innerWidth)
{
    if (percent <= 0 || innerWidth <= 0)
        return 0;
    if (percent >= 100)
        return innerWidth;
    const int width = (percent * innerWidth + 50) / 100;
    return width < 1 ? 1 : width;
}

BatteryMonitor::Color BatteryMonitor::levelColor(int percent)
{
    if (percent <= 15)
        return {225, 80, 80};
    if (percent <= 40)
        return {235, 190, 70};
    return {110, 210, 120};
}

} // namespace miyoofin
