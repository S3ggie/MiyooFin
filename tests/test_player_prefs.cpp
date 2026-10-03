#include "../src/ui/PlayerPrefs.hpp"
#include "test_support.hpp"
#include <cstdio>

using namespace miyoofin;

int main()
{
    const std::string path = "player-prefs-test.txt";
    PlayerPrefs p;
    p.audioLang = "jpn";
    p.subLang = "eng";
    CHECK(p.save(path));
    PlayerPrefs back = PlayerPrefs::load(path);
    CHECK_EQ(back.audioLang, "jpn");
    CHECK_EQ(back.subLang, "eng");
    CHECK(!back.subOff);
    p.subOff = true;
    CHECK(p.save(path));
    CHECK(PlayerPrefs::load(path).subOff);
    CHECK(PlayerPrefs::load("no-such-prefs.txt").audioLang.empty());
    std::remove(path.c_str());
    return miyoofin_test::finish("player_prefs");
}
