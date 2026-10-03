#include "test_support.hpp"

#include "../src/app/ScreenLock.hpp"
#include <cstdio>
#include <fstream>
#include <string>

using namespace miyoofin;

static std::string slurp(const std::string& path)
{
    std::ifstream in(path);
    std::string s;
    std::getline(in, s);
    return s;
}

int main()
{
    const std::string bl = "lock-test-backlight.txt", save = "lock-test-save.txt";
    std::remove(save.c_str());
    {
        std::ofstream(bl) << "80\n";
    }
    ScreenLock lock(bl, save);

    // START + SELECT within the window, either order; one alone never counts.
    CHECK(!lock.comboPressed(Action::Settings, 1000));
    CHECK(lock.comboPressed(Action::Menu, 1300));
    CHECK(!lock.comboPressed(Action::Menu, 5000));
    CHECK(!lock.comboPressed(Action::Settings, 6000)); // too late
    CHECK(!lock.comboPressed(Action::Confirm, 6100));

    CHECK(lock.lock() && lock.locked());
    CHECK_EQ(slurp(bl), "0");
    lock.unlock();
    CHECK(!lock.locked());
    CHECK_EQ(slurp(bl), "80");

    // A crash while locked is undone by recover() on the next start.
    CHECK(lock.lock());
    ScreenLock next(bl, save);
    next.recover();
    CHECK_EQ(slurp(bl), "80");

    // An unreadable backlight is never locked (it could not be restored).
    ScreenLock none("no-such-backlight", save);
    CHECK(!none.lock() && !none.locked());

    std::remove(bl.c_str());
    std::remove(save.c_str());
    return miyoofin_test::finish("screen_lock");
}
