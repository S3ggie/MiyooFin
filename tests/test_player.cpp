#include "test_support.hpp"

#include "../player/osd.h"
#include "../player/subs.h"

#include <vector>

namespace {

struct Frame
{
    int w, h;
    std::vector<uint8_t> y, u, v;
    Frame(int width, int height, uint8_t yy)
        : w(width), h(height), y(width * height, yy),
          u(((width + 1) / 2) * ((height + 1) / 2), 128), v(u.size(), 128)
    {}
    OsdPicture picture(bool rot180)
    {
        OsdPicture p;
        p.data[0] = y.data();
        p.data[1] = u.data();
        p.data[2] = v.data();
        p.linesize[0] = w;
        p.linesize[1] = p.linesize[2] = (w + 1) / 2;
        p.w = w;
        p.h = h;
        p.rot180 = rot180 ? 1 : 0;
        return p;
    }
    uint8_t Y(int x, int yy) const
    {
        return y[yy * w + x];
    }
};

static void testFormatTime()
{
    std::printf("[test] OSD time formatting\n");
    char b[32];
    osd_format_time(0, b, sizeof(b));
    CHECK_EQ(std::string(b), "0:00");
    osd_format_time(59.9, b, sizeof(b));
    CHECK_EQ(std::string(b), "0:59");
    osd_format_time(3725, b, sizeof(b));
    CHECK_EQ(std::string(b), "1:02:05");
    osd_format_time(-4, b, sizeof(b));
    CHECK_EQ(std::string(b), "0:00");
    OsdModel m{30, 120, 0, 1, nullptr};
    CHECK(osd_progress(&m) > 0.249 && osd_progress(&m) < 0.251);
    m.dur_sec = 0;
    CHECK(osd_progress(&m) == 0.0);
    m.dur_sec = 10;
    m.pos_sec = 50;
    CHECK(osd_progress(&m) == 1.0);
    std::printf("[test] OSD time formatting OK\n");
}

static void testBarIsRotatedAndSeekFillTracksProgress()
{
    std::printf("[test] OSD bar rotation and progress fill\n");
    Frame f(640, 360, 120);
    OsdPicture p = f.picture(true);
    OsdModel m{30, 120, 0, 1, nullptr};
    osd_render(&p, &m);
    OsdBarGeometry g = osd_bar_geometry(&p);
    // The panel sits at the LOGICAL bottom, i.e. the picture TOP when rotated.
    CHECK(f.Y(5, 1) < 120);        // darkened panel (picture top rows)
    CHECK(f.Y(5, f.h - 2) == 120); // logical top-left untouched
    // Filled vs unfilled part of the seek bar (logical row g.bar_y + 1).
    int row = f.h - 1 - (g.bar_y + 1);
    int filledX = f.w - 1 - (g.bar_x + g.bar_w / 8);
    int emptyX = f.w - 1 - (g.bar_x + (g.bar_w * 7) / 8);
    CHECK(f.Y(filledX, row) != f.Y(emptyX, row));
    // Without rotation the panel is at the picture bottom.
    Frame g2(640, 360, 120);
    OsdPicture q = g2.picture(false);
    osd_render(&q, &m);
    CHECK(g2.Y(5, g2.h - 2) < 120 && g2.Y(5, 1) == 120);
    std::printf("[test] OSD bar rotation and progress fill OK\n");
}

static void testHiddenBarDrawsNothingAndPauseShowsBadge()
{
    std::printf("[test] OSD hidden bar and pause badge\n");
    Frame f(640, 480, 90);
    OsdPicture p = f.picture(true);
    OsdModel hidden{10, 100, 0, 0, nullptr};
    osd_render(&p, &hidden);
    CHECK(f.y == std::vector<uint8_t>(640 * 480, 90));
    OsdModel paused{10, 100, 1, 0, nullptr};
    osd_render(&p, &paused);
    CHECK(f.Y(320, 240 - 10) != 90 || f.Y(320, 240 + 10) != 90); // centre badge
    CHECK(f.Y(5, 1) < 90);                                       // bar forced on while paused
    std::printf("[test] OSD hidden bar and pause badge OK\n");
}

static void testRenderStaysInsideFootprint()
{
    std::printf("[test] OSD render stays inside its footprint\n");
    for (int rot = 0; rot < 2; ++rot) {
        Frame f(640, 480, 100);
        OsdPicture p = f.picture(rot == 1);
        OsdModel m{61, 7300, 1, 1, "Seek +10s"};
        osd_render(&p, &m);
        int rects[OSD_FOOTPRINT_RECTS][4];
        osd_footprint(&p, rects);
        int outside = 0;
        for (int y = 0; y < f.h; ++y)
            for (int x = 0; x < f.w; ++x) {
                bool inside = false;
                for (auto& r : rects)
                    if (x >= r[0] && x < r[0] + r[2] && y >= r[1] && y < r[1] + r[3])
                        inside = true;
                if (!inside && f.Y(x, y) != 100)
                    ++outside;
            }
        CHECK(outside == 0);
    }
    std::printf("[test] OSD render stays inside its footprint OK\n");
}

static void testLargePictureScalesAndSmallDoesNotCrash()
{
    std::printf("[test] OSD scaling and tiny pictures\n");
    Frame big(1280, 720, 100);
    OsdPicture bp = big.picture(true);
    CHECK(osd_scale(&bp) == 2);
    OsdModel m{1, 2, 1, 1, "x"};
    osd_render(&bp, &m);
    Frame tiny(32, 24, 100); // must clip, not write out of bounds
    OsdPicture tp = tiny.picture(true);
    osd_render(&tp, &m);
    // The track menu stays inside its footprint rectangle (rect 3).
    {
        Frame mf(640, 480, 100);
        OsdPicture mp = mf.picture(true);
        const char* labels[3] = {"Off", "English - Full", "Japanese"};
        int checked[3] = {0, 1, 0};
        osd_menu_render(&mp, "Subtitles", labels, checked, 3, 1);
        int rects[OSD_FOOTPRINT_RECTS][4];
        osd_footprint(&mp, rects);
        int outside = 0, drawn = 0;
        for (int y = 0; y < mf.h; ++y)
            for (int x = 0; x < mf.w; ++x) {
                const int* r = rects[3];
                const bool inside = x >= r[0] && x < r[0] + r[2] && y >= r[1] && y < r[1] + r[3];
                if (mf.Y(x, y) != 100) {
                    ++drawn;
                    if (!inside)
                        ++outside;
                }
            }
        CHECK(drawn > 1000 && outside == 0);
    }
    std::printf("[test] OSD scaling and tiny pictures OK\n");
}

static void testSrtParsing()
{
    std::printf("[test] SRT parsing and cue lookup\n");
    const std::string srt =
        "\xEF\xBB\xBF"
        "1\r\n00:00:01,000 --> 00:00:03,500\r\n<i>Hello</i> {\\an8}world\r\n\r\n"
        "2\r\n00:00:03,000 --> 00:00:04,000\r\nLine one\\NLine two\r\nand three\r\n\r\n"
        "garbage line\r\n\r\n"
        "00:01:00.5 --> 00:01:02.25\r\nno index, short fraction\r\n\r\n"
        "4\r\n00:02:00,000 --> 00:02:01,000\r\n<b></b>\r\n";
    SubCues c;
    CHECK(subs_parse_srt(srt.data(), srt.size(), &c) == 3); // the empty-markup cue is dropped
    CHECK(c.cues[0].start_ms == 1000 && c.cues[0].end_ms == 3500);
    CHECK(std::string(c.cues[0].text) == "Hello world");
    CHECK(std::string(c.cues[1].text) == "Line one\nLine two\nand three");
    CHECK(c.cues[2].start_ms == 60500 && c.cues[2].end_ms == 62250); // ".5" is 500 ms
    CHECK(subs_find(&c, 500) == -1);
    CHECK(subs_find(&c, 1000) == 0);
    CHECK(subs_find(&c, 3200) == 1); // overlap: later-starting cue wins
    CHECK(subs_find(&c, 3800) == 1);
    CHECK(subs_find(&c, 4000) == -1); // end is exclusive
    CHECK(subs_find(&c, 61000) == 2);
    CHECK(subs_find(&c, 999999) == -1);
    subs_free(&c);
    // Out-of-order input is sorted; junk yields nothing.
    const std::string rev =
        "1\n00:00:09,000 --> 00:00:10,000\nb\n\n2\n00:00:01,000 --> 00:00:02,000\na\n";
    CHECK(subs_parse_srt(rev.data(), rev.size(), &c) == 2 && std::string(c.cues[0].text) == "a");
    subs_free(&c);
    CHECK(subs_parse_srt("hello", 5, &c) == 0);
    CHECK(subs_parse_srt("", 0, &c) == 0);
    std::printf("[test] SRT parsing and cue lookup OK\n");
}

static void testTrackFileParsing()
{
    std::printf("[test] track list file parsing\n");
    const std::string file = "a|1|0|1|0|eng|English - Dolby\ns|3|1|0|0|eng|English   SDH\n"
                             "s|4|0|1|0|fra|\nbogus line\ns|5|1|0|1|jpn|Japanese\n";
    SubTrackInfo t[8];
    int n = subs_parse_tracks(file.data(), file.size(), t, 8);
    CHECK(n == 4);
    CHECK(t[0].type == 'a' && t[0].index == 1 && t[0].is_default);
    CHECK(t[1].type == 's' && t[1].index == 3 && t[1].text &&
          std::string(t[1].title) == "English   SDH");
    CHECK(t[2].index == 4 && !t[2].text && std::string(t[2].title).empty());
    CHECK(t[3].forced && std::string(t[3].lang) == "jpn");
    CHECK(subs_parse_tracks(file.data(), file.size(), t, 2) == 2); // bounded
    std::printf("[test] track list file parsing OK\n");
}

static void testBlendArgbIsRotatedAndClipped()
{
    std::printf("[test] ARGB blit onto YUV\n");
    Frame f(64, 48, 100);
    OsdPicture p = f.picture(true);
    uint8_t px[4 * 4 * 4];
    for (int i = 0; i < 16; ++i) { // 4x4 opaque white
        px[i * 4 + 0] = 255;
        px[i * 4 + 1] = 255;
        px[i * 4 + 2] = 255;
        px[i * 4 + 3] = 255;
    }
    osd_blit_rgba(&p, 2, 3, px, 4, 4, 16);    // logical (2,3)
    CHECK(f.Y(64 - 1 - 2, 48 - 1 - 3) > 200); // rotated destination
    CHECK(f.Y(2, 3) == 100);
    // Fully transparent pixels leave the picture untouched; off-screen is clipped.
    for (int i = 0; i < 16; ++i)
        px[i * 4 + 3] = 0;
    Frame g(64, 48, 100);
    OsdPicture q = g.picture(false);
    osd_blit_rgba(&q, 10, 10, px, 4, 4, 16);
    osd_blit_rgba(&q, -3, -3, px, 4, 4, 16);
    osd_blit_rgba(&q, 62, 46, px, 4, 4, 16);
    CHECK(g.y == std::vector<uint8_t>(64 * 48, 100));
    for (int i = 0; i < 16; ++i)
        px[i * 4 + 3] = 255;
    osd_blit_rgba(&q, 62, 46, px, 4, 4, 16); // partially off the corner: no overrun
    CHECK(g.Y(63, 47) > 200);
    std::printf("[test] ARGB blit onto YUV OK\n");
}

static void testLanguagePreferences()
{
    std::printf("[test] remembered language preferences\n");
    SubPrefs p;
    subs_prefs_parse("sub_lang=eng\nsub_off=0\naudio_lang=jpn\njunk\n", 43, &p);
    CHECK(std::string(p.sub_lang) == "eng" && !p.sub_off && std::string(p.audio_lang) == "jpn");
    char buf[96];
    subs_prefs_format(&p, buf, sizeof(buf));
    SubPrefs q;
    subs_prefs_parse(buf, strlen(buf), &q);
    CHECK(std::string(q.sub_lang) == "eng" && std::string(q.audio_lang) == "jpn");
    subs_prefs_parse("sub_off=1\n", 11, &q);
    CHECK(q.sub_off == 1 && q.sub_lang[0] == 0);

    // Tracks: audio jpn(default) eng; subtitles eng-signs(forced,default) eng-full spa.
    const std::string file = "a|2|0|1|0|jpn|Japanese\na|3|0|0|0|eng|English\n"
                             "s|1|1|1|1|eng|Signs\ns|0|1|0|0|eng|Full\ns|4|1|0|0|spa|Spanish\n";
    SubTrackInfo t[8];
    int n = subs_parse_tracks(file.data(), file.size(), t, 8);
    CHECK(n == 5);
    int ok[8];
    for (int i = 0; i < n; i++)
        ok[i] = t[i].type == 's' && t[i].text;
    CHECK(subs_pick_auto(t, n, 'a', "eng", nullptr) == 1);  // remembered language wins
    CHECK(subs_pick_auto(t, n, 'a', "", nullptr) == 0);     // else the file's default
    CHECK(subs_pick_auto(t, n, 'a', "fra", nullptr) == -1); // remembered but absent
    CHECK(subs_pick_auto(t, n, 's', "eng", ok) == 3);       // full English beats forced signs
    CHECK(subs_pick_auto(t, n, 's', "spa", ok) == 4);
    CHECK(subs_pick_auto(t, n, 's', "", ok) == 2); // no preference: default flag
    std::printf("[test] remembered language preferences OK\n");
}

} // namespace

int main()
{
    testFormatTime();
    testBarIsRotatedAndSeekFillTracksProgress();
    testHiddenBarDrawsNothingAndPauseShowsBadge();
    testRenderStaysInsideFootprint();
    testLargePictureScalesAndSmallDoesNotCrash();
    testSrtParsing();
    testTrackFileParsing();
    testBlendArgbIsRotatedAndClipped();
    testLanguagePreferences();
    return miyoofin_test::finish("player");
}
