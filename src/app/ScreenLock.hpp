#ifndef MIYOOFIN_SCREEN_LOCK_HPP
#define MIYOOFIN_SCREEN_LOCK_HPP

#include "../input/Action.hpp"
#include <cstdio>
#include <string>

namespace miyoofin {

/// Screen-off listening: the backlight goes dark and every button is ignored until
/// START + SELECT are pressed together again. The backlight value is kept in a file
/// so a crash while locked is undone the next time the app starts.
class ScreenLock
{
  public:
    static constexpr const char* kBacklight = "/sys/class/pwm/pwmchip0/pwm0/duty_cycle";
    static constexpr int kComboWindowMs = 500;

    ScreenLock(std::string backlightPath = kBacklight, std::string savePath = "screen-lock.txt")
        : m_backlight(std::move(backlightPath)), m_save(std::move(savePath))
    {}

    bool locked() const
    {
        return m_locked;
    }

    /// Call for every action. True when START and SELECT were both pressed within the
    /// combo window (in either order).
    bool comboPressed(Action a, long nowMs)
    {
        if (a != Action::Settings && a != Action::Menu)
            return false;
        long& mine = a == Action::Settings ? m_startAt : m_selectAt;
        const long other = a == Action::Settings ? m_selectAt : m_startAt;
        mine = nowMs;
        if (other >= 0 && nowMs - other <= kComboWindowMs) {
            m_startAt = m_selectAt = -1;
            return true;
        }
        return false;
    }

    /// Backlight off. False (nothing changes) when its current value cannot be read.
    bool lock()
    {
        if (m_locked)
            return true;
        const long now = read();
        if (now <= 0)
            return false;
        FILE* f = std::fopen(m_save.c_str(), "w");
        if (f) {
            std::fprintf(f, "%ld\n", now);
            std::fclose(f);
        }
        m_locked = write(0);
        return m_locked;
    }

    void unlock()
    {
        if (m_locked)
            restore();
        m_locked = false;
    }

    /// At startup: undo a lock left behind by a crash.
    void recover()
    {
        restore();
    }

  private:
    long read() const
    {
        long v = -1;
        FILE* f = std::fopen(m_backlight.c_str(), "r");
        if (f) {
            if (std::fscanf(f, "%ld", &v) != 1)
                v = -1;
            std::fclose(f);
        }
        return v;
    }
    bool write(long v) const
    {
        FILE* f = std::fopen(m_backlight.c_str(), "w");
        if (!f)
            return false;
        std::fprintf(f, "%ld\n", v);
        return std::fclose(f) == 0;
    }
    void restore()
    {
        long v = -1;
        FILE* f = std::fopen(m_save.c_str(), "r");
        if (!f)
            return;
        if (std::fscanf(f, "%ld", &v) != 1)
            v = -1;
        std::fclose(f);
        if (v > 0)
            write(v);
        std::remove(m_save.c_str());
    }

    std::string m_backlight, m_save;
    bool m_locked = false;
    long m_startAt = -1, m_selectAt = -1;
};

} // namespace miyoofin

#endif
