#ifndef MIYOOFIN_CRASH_REPORT_SCREEN_HPP
#define MIYOOFIN_CRASH_REPORT_SCREEN_HPP

#include "../../app/CrashReport.hpp"
#include "../../app/Screen.hpp"

namespace miyoofin {

/// Settings > Diagnostics: the newest entry of crash.log. X clears the log, B goes back.
class CrashReportScreen : public Screen
{
  public:
    CrashReportScreen();
    bool handleAction(Action action) override;
    void update(Uint32) override {}
    void render(SDL_Surface* fb) override;
    const char* diagnosticName() const override
    {
        return "CrashReportScreen";
    }

  private:
    CrashReport m_report;
};

} // namespace miyoofin

#endif
