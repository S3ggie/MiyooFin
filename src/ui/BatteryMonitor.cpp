#include "BatteryMonitor.hpp"

#include "../diagnostics/UiDiagnostics.hpp"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <fcntl.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace miyoofin {

BatteryMonitor::~BatteryMonitor()
{
    // The probe is bounded (it polls the cancel token every 50ms and kills the
    // child), so this join cannot wait on a hung power-chip query.
    m_probe.cancel();
    m_probe.join();
}

void BatteryMonitor::update(unsigned dtMs)
{
    // --- level (/tmp/percBat) ---
    bool readLevel = !m_hasRead;
    if (m_hasRead) {
        m_sinceReadMs += dtMs;
        readLevel = m_sinceReadMs >= kRefreshMs;
    }
    if (readLevel) {
        m_hasRead = true;
        m_sinceReadMs = 0;
        m_percent = -1;
        std::FILE* file = std::fopen(m_path.c_str(), "r");
        if (file) {
            char buffer[16] = {};
            const std::size_t n = std::fread(buffer, 1, sizeof(buffer) - 1, file);
            std::fclose(file);
            m_percent = parsePercent(std::string(buffer, n));
        }
    }

    // --- charging (axp_test on a worker) ---
    if (m_probe.reap()) {
        if (m_probeExecFailed)
            m_probeUnavailable = true; // not found / not executable: stop trying
        m_charging = parseCharging(m_probeOutput);
        if (m_charging != m_loggedCharging) {
            m_loggedCharging = m_charging;
            char line[64];
            std::snprintf(line, sizeof(line), "[Battery] charging=%d percent=%d", m_charging,
                          m_percent);
            std::printf("%s\n", line);
            uiDiagnostics().log(line); // also reaches ui-stall.log on a normal launch
        }
    }
    m_sinceProbeMs += dtMs;
    if (!m_probeUnavailable && !m_probe.busy() &&
        (m_neverProbed || m_sinceProbeMs >= kChargingRefreshMs)) {
        m_neverProbed = false;
        m_sinceProbeMs = 0;
        m_probeOutput.clear();
        m_probeExecFailed = false;
        m_probe.start([this](const CancelToken& cancel) { runProbe(cancel); });
    }
}

void BatteryMonitor::runProbe(const CancelToken& cancel)
{
    // Everything the child needs is prepared BEFORE fork(): after fork() in a
    // multithreaded process only async-signal-safe calls are allowed.
    const std::size_t slash = m_axpPath.rfind('/');
    const std::string dir = slash == std::string::npos ? "." : m_axpPath.substr(0, slash);
    const std::string base = slash == std::string::npos ? m_axpPath : m_axpPath.substr(slash + 1);

    int fds[2];
    if (pipe(fds) != 0)
        return;
    const pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return;
    }
    if (pid == 0) {
        dup2(fds[1], STDOUT_FILENO);
        const int devNull = open("/dev/null", O_WRONLY);
        if (devNull >= 0)
            dup2(devNull, STDERR_FILENO);
        close(fds[0]);
        close(fds[1]);
        // axp_test expects to run from its own directory (batmon cd's there).
        if (chdir(dir.c_str()) != 0)
            _exit(127);
        execl(m_axpPath.c_str(), base.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }

    close(fds[1]);
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(m_probeTimeoutMs);
    bool finished = false;
    while (!cancel->load(std::memory_order_relaxed) &&
           std::chrono::steady_clock::now() < deadline) {
        pollfd pfd{fds[0], POLLIN, 0};
        const int ready = poll(&pfd, 1, 50);
        if (ready > 0) {
            char buffer[256];
            const ssize_t n = read(fds[0], buffer, sizeof(buffer));
            if (n > 0) {
                if (m_probeOutput.size() < 1024)
                    m_probeOutput.append(buffer, static_cast<std::size_t>(n));
            } else if (n == 0 || (n < 0 && errno != EINTR)) {
                finished = true; // EOF: the child closed stdout (exited)
                break;
            }
        }
    }
    close(fds[0]);
    if (!finished)
        kill(pid, SIGKILL); // timeout or cancellation: never leave it running
    int status = 0;
    waitpid(pid, &status, 0);
    m_probeExecFailed = WIFEXITED(status) && WEXITSTATUS(status) == 127;
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

int BatteryMonitor::parseCharging(const std::string& output)
{
    const std::string key = "\"charging\"";
    const std::size_t at = output.find(key);
    if (at == std::string::npos)
        return -1;
    std::size_t i = output.find(':', at + key.size());
    if (i == std::string::npos)
        return -1;
    ++i;
    while (i < output.size() && (output[i] == ' ' || output[i] == '\t'))
        ++i;
    if (i >= output.size())
        return -1;
    const char value = output[i];
    // Exactly 0 or 1, not followed by more digits.
    if ((value != '0' && value != '1') ||
        (i + 1 < output.size() && output[i + 1] >= '0' && output[i + 1] <= '9'))
        return -1;
    return value - '0';
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
