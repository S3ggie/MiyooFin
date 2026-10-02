#include "test_support.hpp"

#include "../player/osd.h"

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
    std::printf("[test] OSD scaling and tiny pictures OK\n");
}

} // namespace

int main()
{
    testFormatTime();
    testBarIsRotatedAndSeekFillTracksProgress();
    testHiddenBarDrawsNothingAndPauseShowsBadge();
    testRenderStaysInsideFootprint();
    testLargePictureScalesAndSmallDoesNotCrash();
    return miyoofin_test::finish("player");
}
