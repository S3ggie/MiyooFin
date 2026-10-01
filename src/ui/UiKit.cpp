#include "UiKit.hpp"

#include "BatteryMonitor.hpp"
#include "BitmapFont.hpp"

#include <algorithm>
#include <cmath>
#include <ctime>

namespace miyoofin {
namespace ui {

using namespace design;

// ---------------------------------------------------------------- primitives

void fill(SDL_Surface* fb, int x, int y, int w, int h, Rgb c)
{
    BitmapFont::fillRect(fb, x, y, w, h, c.r, c.g, c.b, 255);
}

void blend(SDL_Surface* fb, int x, int y, int w, int h, Rgb c, int alpha)
{
    if (!fb || fb->format->BytesPerPixel != 4)
        return;
    alpha = std::max(0, std::min(255, alpha));
    if (alpha == 0)
        return;
    if (alpha == 255) {
        fill(fb, x, y, w, h, c);
        return;
    }
    const int x0 = std::max(0, x), y0 = std::max(0, y);
    const int x1 = std::min(fb->w, x + w), y1 = std::min(fb->h, y + h);
    if (x0 >= x1 || y0 >= y1)
        return;
    const SDL_PixelFormat* f = fb->format;
    const Uint32 keep = ~(f->Rmask | f->Gmask | f->Bmask);
    const int inv = 255 - alpha;
    for (int py = y0; py < y1; ++py) {
        Uint32* row = reinterpret_cast<Uint32*>(static_cast<Uint8*>(fb->pixels) + py * fb->pitch);
        for (int px = x0; px < x1; ++px) {
            const Uint32 p = row[px];
            const int r = ((p & f->Rmask) >> f->Rshift);
            const int g = ((p & f->Gmask) >> f->Gshift);
            const int b = ((p & f->Bmask) >> f->Bshift);
            const Uint32 nr = static_cast<Uint32>((c.r * alpha + r * inv) / 255);
            const Uint32 ng = static_cast<Uint32>((c.g * alpha + g * inv) / 255);
            const Uint32 nb = static_cast<Uint32>((c.b * alpha + b * inv) / 255);
            row[px] = (p & keep) | (nr << f->Rshift) | (ng << f->Gshift) | (nb << f->Bshift);
        }
    }
}

void outline(SDL_Surface* fb, int x, int y, int w, int h, Rgb c)
{
    BitmapFont::drawRect(fb, x, y, w, h, c.r, c.g, c.b);
}

Rgb mix(Rgb a, Rgb b, int percentOfB)
{
    percentOfB = std::max(0, std::min(100, percentOfB));
    const int pa = 100 - percentOfB;
    return {static_cast<std::uint8_t>((a.r * pa + b.r * percentOfB) / 100),
            static_cast<std::uint8_t>((a.g * pa + b.g * percentOfB) / 100),
            static_cast<std::uint8_t>((a.b * pa + b.b * percentOfB) / 100)};
}

// Horizontal inset of corner row `i` (0 = outermost row) for radius `r`.
static int cornerInset(int i, int r)
{
    if (r <= 0 || i >= r)
        return 0;
    const double dy = r - i - 0.5;
    const double dx = std::sqrt(static_cast<double>(r) * r - dy * dy);
    return std::max(0, static_cast<int>(std::lround(r - dx)));
}

static int rowInset(int row, int h, int r)
{
    if (row < r)
        return cornerInset(row, r);
    if (row >= h - r)
        return cornerInset(h - 1 - row, r);
    return 0;
}

void roundFill(SDL_Surface* fb, int x, int y, int w, int h, int radius, Rgb c)
{
    if (w <= 0 || h <= 0)
        return;
    radius = std::min(radius, std::min(w, h) / 2);
    if (radius <= 0) {
        fill(fb, x, y, w, h, c);
        return;
    }
    for (int row = 0; row < h; ++row) {
        const int inset = rowInset(row, h, radius);
        fill(fb, x + inset, y + row, w - 2 * inset, 1, c);
    }
}

void roundOutline(SDL_Surface* fb, int x, int y, int w, int h, int radius, Rgb c)
{
    if (w <= 1 || h <= 1)
        return;
    radius = std::min(radius, std::min(w, h) / 2);
    for (int row = 0; row < h; ++row) {
        const int outer = rowInset(row, h, radius);
        if (row == 0 || row == h - 1) {
            fill(fb, x + outer, y + row, w - 2 * outer, 1, c);
            continue;
        }
        // Inner boundary comes from the shape inset by one pixel.
        const int inner = 1 + rowInset(row - 1, h - 2, std::max(0, radius - 1));
        const int innerClamped = std::max(inner, outer + 1);
        fill(fb, x + outer, y + row, innerClamped - outer, 1, c);
        fill(fb, x + w - innerClamped, y + row, innerClamped - outer, 1, c);
    }
}

void gradientV(SDL_Surface* fb, int x, int y, int w, int h, Rgb top, Rgb bottom)
{
    if (h <= 0)
        return;
    for (int row = 0; row < h; ++row)
        fill(fb, x, y + row, w, 1, mix(top, bottom, h <= 1 ? 0 : row * 100 / (h - 1)));
}

// ---------------------------------------------------------------------- text

int textWidth(const std::string& s, int scale)
{
    return BitmapFont::glyphCount(s) * BitmapFont::GLYPH_W * scale;
}

int text(SDL_Surface* fb, int x, int y, const std::string& s, Rgb c, int scale)
{
    return BitmapFont::drawStringTransparent(fb, x, y, s.c_str(), scale, c.r, c.g, c.b);
}

int textShadow(SDL_Surface* fb, int x, int y, const std::string& s, Rgb c, int scale)
{
    BitmapFont::drawStringTransparent(fb, x + scale, y + scale, s.c_str(), scale, 0, 0, 0);
    return text(fb, x, y, s, c, scale);
}

std::string fit(const std::string& s, int maxWidthPx, int scale)
{
    const int glyphW = BitmapFont::GLYPH_W * scale;
    return BitmapFont::truncateUtf8(s, std::max(0, maxWidthPx / glyphW));
}

int textClamped(SDL_Surface* fb, int x, int y, int maxWidthPx, const std::string& s, Rgb c,
                int scale)
{
    return text(fb, x, y, fit(s, maxWidthPx, scale), c, scale);
}

std::vector<std::string> wrap(const std::string& s, int maxWidthPx, int maxLines, int scale)
{
    std::vector<std::string> lines;
    const int maxGlyphs = std::max(1, maxWidthPx / (BitmapFont::GLYPH_W * scale));
    if (maxLines <= 0)
        return lines;
    std::string line;
    std::size_t pos = 0;
    auto flush = [&] {
        lines.push_back(line);
        line.clear();
    };
    while (pos < s.size()) {
        std::size_t end = s.find(' ', pos);
        if (end == std::string::npos)
            end = s.size();
        std::string word = s.substr(pos, end - pos);
        pos = end + 1;
        if (word.empty())
            continue;
        const int wordGlyphs = BitmapFont::glyphCount(word);
        const int lineGlyphs = BitmapFont::glyphCount(line);
        if (!line.empty() && lineGlyphs + 1 + wordGlyphs > maxGlyphs) {
            flush();
            if (static_cast<int>(lines.size()) == maxLines) {
                // More text remains than fits: end the last line with "..".
                std::string last = lines.back() + " " + word +
                                   (pos < s.size() ? " " + s.substr(pos) : std::string());
                lines.back() = BitmapFont::truncateUtf8(last, maxGlyphs);
                return lines;
            }
        }
        if (wordGlyphs > maxGlyphs) { // a single over-long word: hard cut
            if (!line.empty())
                flush();
            word = BitmapFont::truncateUtf8(word, maxGlyphs);
        }
        line += (line.empty() ? "" : " ") + word;
    }
    if (!line.empty() && static_cast<int>(lines.size()) < maxLines)
        flush();
    return lines;
}

// ------------------------------------------------------------------ controls

void focusRing(SDL_Surface* fb, int x, int y, int w, int h, int radius)
{
    // Layered rounded frames approximate a soft electric-blue glow.
    roundOutline(fb, x - 4, y - 4, w + 8, h + 8, radius + 4, kAccentDim);
    roundOutline(fb, x - 3, y - 3, w + 6, h + 6, radius + 3, Rgb{34, 95, 180});
    roundOutline(fb, x - 2, y - 2, w + 4, h + 4, radius + 2, kAccent);
    roundOutline(fb, x - 1, y - 1, w + 2, h + 2, radius + 1, Rgb{90, 175, 255});
    roundOutline(fb, x, y, w, h, radius, kAccentHi);
}

void button(SDL_Surface* fb, int x, int y, int w, int h, const std::string& label,
            ButtonStyle style, bool focused, bool enabled)
{
    Rgb surface = kRaised, border = kBorder, fg = kText;
    switch (style) {
    case ButtonStyle::Primary:
        surface = focused ? kAccent : kAccentDim;
        border = focused ? kAccentHi : kAccent;
        break;
    case ButtonStyle::Secondary:
        surface = focused ? kAccentSoft : kRaised;
        border = focused ? kAccent : kBorder;
        break;
    case ButtonStyle::Danger:
        surface = focused ? Rgb{110, 28, 40} : kPanel;
        border = focused ? kDanger : Rgb{120, 50, 58};
        fg = focused ? Rgb{255, 235, 235} : Rgb{235, 150, 150};
        break;
    }
    if (!enabled) {
        surface = kPanel;
        border = kDivider;
        fg = kTextMuted;
    }
    if (focused && enabled)
        focusRing(fb, x, y, w, h);
    roundFill(fb, x, y, w, h, kRadius, surface);
    roundOutline(fb, x, y, w, h, kRadius, border);
    const std::string shown = fit(label, w - 12);
    const int tx = x + (w - textWidth(shown)) / 2;
    const int ty = y + (h - BitmapFont::GLYPH_H) / 2;
    text(fb, tx, ty, shown, fg);
}

int chip(SDL_Surface* fb, int x, int y, const std::string& label, Rgb bg, Rgb fg)
{
    const int w = textWidth(label) + 12;
    roundFill(fb, x, y, w, 20, 3, bg);
    text(fb, x + 6, y + 2, label, fg);
    return w;
}

void progressBar(SDL_Surface* fb, int x, int y, int w, int h, int percent, Rgb fillColor)
{
    roundFill(fb, x, y, w, h, std::min(2, h / 2), kBorder);
    percent = std::max(0, std::min(100, percent));
    const int fw = w * percent / 100;
    if (fw > 0)
        roundFill(fb, x, y, std::max(fw, std::min(w, 2 * std::min(2, h / 2))), h,
                  std::min(2, h / 2), fillColor);
}

void panel(SDL_Surface* fb, int x, int y, int w, int h, Rgb surface, Rgb border)
{
    roundFill(fb, x, y, w, h, kRadius, border);
    roundFill(fb, x + 1, y + 1, w - 2, h - 2, std::max(0, kRadius - 1), surface);
}

void field(SDL_Surface* fb, int x, int y, int w, int h, const std::string& label,
           const std::string& value, bool focused)
{
    if (focused)
        focusRing(fb, x, y, w, h);
    roundFill(fb, x, y, w, h, kRadius, focused ? kAccent : kBorder);
    roundFill(fb, x + 1, y + 1, w - 2, h - 2, kRadius - 1, kPanel);
    text(fb, x + 10, y + 5, label, focused ? kAccentHi : kTextMuted);
    const std::string shown = fit(value, w - 20);
    text(fb, x + 10, y + h - BitmapFont::GLYPH_H - 5, shown, kText);
}

// --------------------------------------------------------------------- icons

void iconBattery(SDL_Surface* fb, int x, int y, int percent, bool charging)
{
    roundOutline(fb, x, y, 22, 11, 2, Rgb{190, 200, 215});
    fill(fb, x + 22, y + 3, 2, 5, Rgb{190, 200, 215});
    const int fw = BatteryMonitor::fillWidth(percent, 18);
    if (fw > 0) {
        const auto lc = BatteryMonitor::levelColor(percent);
        fill(fb, x + 2, y + 2, fw, 7, Rgb{lc.r, lc.g, lc.b});
    }
    if (charging) {
        // 5x7 bolt, columns per row [from, to] inclusive.
        static const int kBolt[7][2] = {{2, 3}, {1, 2}, {0, 3}, {1, 4}, {2, 3}, {1, 2}, {0, 0}};
        const int bx = x + 2 + 6;
        for (int r = 0; r < 7; ++r)
            fill(fb, bx + kBolt[r][0], y + 2 + r, kBolt[r][1] - kBolt[r][0] + 1, 1,
                 Rgb{255, 255, 255});
    }
}

void iconPlay(SDL_Surface* fb, int x, int y, int size, Rgb c)
{
    // Right-pointing triangle: row i is as wide as its distance from the apex.
    const int half = size / 2;
    for (int i = 0; i < size; ++i) {
        const int d = i <= half ? i : size - 1 - i;
        fill(fb, x, y + i, 1 + d * size / (2 * std::max(1, half)), 1, c);
    }
}

void iconDownload(SDL_Surface* fb, int x, int y, int size, Rgb c)
{
    const int mid = x + size / 2;
    const int shaft = std::max(2, size / 6);
    fill(fb, mid - shaft / 2, y, shaft, size * 5 / 8, c);
    // Arrow head
    const int headTop = y + size * 3 / 8;
    for (int i = 0; i < size / 2; ++i)
        fill(fb, mid - (size / 2 - 1 - i), headTop + i, 2 * (size / 2 - 1 - i) + 1, 1, c);
    // Tray
    fill(fb, x, y + size - std::max(2, size / 6), size, std::max(2, size / 6), c);
}

void iconCheck(SDL_Surface* fb, int x, int y, int size, Rgb c)
{
    const int third = std::max(2, size / 3);
    for (int i = 0; i < third; ++i)
        fill(fb, x + i, y + size / 2 + i - 1, 2, 2, c);
    for (int i = 0; i < size - third; ++i)
        fill(fb, x + third + i, y + size / 2 + third - 2 - i, 2, 2, c);
}

void iconChevron(SDL_Surface* fb, int x, int y, int size, bool pointRight, Rgb c)
{
    const int half = size / 2;
    for (int i = 0; i <= half; ++i) {
        const int cx = pointRight ? x + i : x + half - i;
        fill(fb, cx, y + i, 2, 1, c);
        fill(fb, cx, y + size - 1 - i, 2, 1, c);
    }
}

void statusDot(SDL_Surface* fb, int x, int y, int size, Rgb c)
{
    roundFill(fb, x, y, size, size, size / 2, c);
}

// -------------------------------------------------------------------- chrome

static void badgeBox(SDL_Surface* fb, int x, int y, int w, const char* label, Rgb bg, Rgb fg)
{
    roundFill(fb, x, y, w, 16, 4, bg);
    text(fb, x + (w - static_cast<int>(std::char_traits<char>::length(label)) * 8) / 2, y, label,
         fg);
}

int keyBadge(SDL_Surface* fb, int x, int y, Key key)
{
    switch (key) {
    case Key::Dpad:
        fill(fb, x + 6, y, 6, 16, Rgb{150, 156, 172});
        fill(fb, x + 1, y + 5, 16, 6, Rgb{150, 156, 172});
        fill(fb, x + 7, y + 6, 4, 4, kFooterBg);
        return 18;
    case Key::A:
        badgeBox(fb, x, y, 16, "A", Rgb{45, 120, 235}, kText);
        return 16;
    case Key::B:
        badgeBox(fb, x, y, 16, "B", Rgb{205, 70, 80}, kText);
        return 16;
    case Key::X:
        badgeBox(fb, x, y, 16, "X", Rgb{190, 140, 40}, kText);
        return 16;
    case Key::Y:
        badgeBox(fb, x, y, 16, "Y", Rgb{50, 150, 95}, kText);
        return 16;
    case Key::L:
        badgeBox(fb, x, y, 20, "L", Rgb{58, 66, 90}, kText);
        return 20;
    case Key::R:
        badgeBox(fb, x, y, 20, "R", Rgb{58, 66, 90}, kText);
        return 20;
    case Key::LR:
        badgeBox(fb, x, y, 20, "L", Rgb{58, 66, 90}, kText);
        badgeBox(fb, x + 22, y, 20, "R", Rgb{58, 66, 90}, kText);
        return 42;
    case Key::Start:
        badgeBox(fb, x, y, 48, "START", Rgb{58, 66, 90}, kText);
        return 48;
    case Key::Select:
        badgeBox(fb, x, y, 56, "SELECT", Rgb{58, 66, 90}, kText);
        return 56;
    }
    return 0;
}

static void drawWordmark(SDL_Surface* fb, int x, int y)
{
    // Two-tone wordmark at 2x: "Miyoo" in near-white, "Fin" in the accent.
    const int w = text(fb, x, y, "Miyoo", kText, 2);
    text(fb, x + w, y, "Fin", kAccent, 2);
}

void header(SDL_Surface* fb, const HeaderSpec& spec)
{
    fill(fb, 0, 0, kScreenW, kHeaderH, kHeaderBg);
    fill(fb, 0, kHeaderH - 1, kScreenW, 1, kAccentDim);
    drawWordmark(fb, 16, (kHeaderH - 32) / 2);

    const int contentLeft = 16 + 8 * 8 * 2 + 20; // after the 128px wordmark
    int statusLeft = kScreenW - kMargin;
    if (spec.showStatus) {
        std::time_t now = std::time(nullptr);
        std::tm local{};
        localtime_r(&now, &local);
        char clock[8];
        std::snprintf(clock, sizeof(clock), "%02d:%02d", local.tm_hour, local.tm_min);
        const int clockX = kScreenW - kMargin - 40;
        text(fb, clockX, (kHeaderH - 16) / 2, clock, kText);
        const int batteryX = clockX - 12 - 24;
        iconBattery(fb, batteryX, (kHeaderH - 11) / 2, spec.batteryPercent, spec.charging);
        statusLeft = batteryX - 12;
    }

    if (!spec.tabs.empty()) {
        int x = kTabsLeft;
        for (std::size_t i = 0; i < spec.tabs.size(); ++i) {
            const std::string& name = spec.tabs[i];
            const int w = textWidth(name) + 2 * kTabPad;
            const bool active = static_cast<int>(i) == spec.activeTab;
            if (active) {
                roundFill(fb, x, kTabTop, w, kTabHeight, 5, kAccent);
                text(fb, x + kTabPad, kTabTop + 4, name, Rgb{255, 255, 255});
            } else {
                text(fb, x + kTabPad, kTabTop + 4, name, kTextSecondary);
            }
            x += w + kTabGap;
        }
    } else if (!spec.title.empty()) {
        textClamped(fb, contentLeft, (kHeaderH - 16) / 2, statusLeft - contentLeft, spec.title,
                    kTextSecondary);
    }
}

void footer(SDL_Surface* fb, const FooterSpec& spec)
{
    const int y = kScreenH - kFooterH;
    fill(fb, 0, y, kScreenW, kFooterH, kFooterBg);
    fill(fb, 0, y, kScreenW, 1, kDivider);
    int x = kMargin;
    const int by = y + (kFooterH - 16) / 2;
    const bool prompt = !spec.message.empty();
    if (prompt) {
        // A confirmation prompt owns the whole bar.
        statusDot(fb, x, y + (kFooterH - 8) / 2, 8, spec.messageColor);
        textClamped(fb, x + 16, by, kScreenW - x - 2 * kMargin - 16, spec.message,
                    spec.messageColor);
        return;
    }
    for (const Hint& hint : spec.hints) {
        x += keyBadge(fb, x, by, hint.key) + 6;
        x += text(fb, x, by, hint.label, kTextSecondary) + 16;
    }

    // Right side, built right to left: link status, then the optional note.
    int rightEdge = kScreenW - kMargin;
    if (spec.showLink && !spec.rightText.empty()) {
        Rgb dot = kTextMuted;
        switch (spec.link) {
        case LinkState::Connected:
            dot = kSuccess;
            break;
        case LinkState::Offline:
            dot = kDanger;
            break;
        case LinkState::Checking:
            dot = kWarning;
            break;
        case LinkState::Unknown:
            break;
        }
        const std::string shown = fit(spec.rightText, std::max(0, rightEdge - x - 24));
        const int w = textWidth(shown);
        text(fb, rightEdge - w, by, shown, kTextSecondary);
        statusDot(fb, rightEdge - w - 14, y + (kFooterH - 8) / 2, 8, dot);
        rightEdge -= w + 14 + 14;
    }
    if (!spec.note.empty() && rightEdge - x > 24) {
        const std::string shown = fit(spec.note, rightEdge - x - 8);
        text(fb, rightEdge - textWidth(shown), by, shown, spec.noteColor);
    }
}

// ------------------------------------------------------------------- artwork

SDL_Surface* coverSurface(const DecodedImage& img, int w, int h)
{
    if (img.empty() || w <= 0 || h <= 0)
        return nullptr;
    SDL_Surface* out =
        SDL_CreateRGBSurface(0, w, h, 32, 0x000000FF, 0x0000FF00, 0x00FF0000, 0xFF000000);
    if (!out)
        return nullptr;

    // Largest source window with the target's aspect ratio, centred.
    const double ia = static_cast<double>(img.width) / img.height;
    const double ba = static_cast<double>(w) / h;
    double srcW = img.width, srcH = img.height;
    if (ia > ba)
        srcW = img.height * ba;
    else
        srcH = img.width / ba;
    const double srcX = (img.width - srcW) / 2.0;
    const double srcY = (img.height - srcH) / 2.0;
    const double stepX = srcW / w, stepY = srcH / h;

    const unsigned char* src = img.pixels.data();
    SDL_LockSurface(out);
    for (int dy = 0; dy < h; ++dy) {
        Uint32* row = reinterpret_cast<Uint32*>(static_cast<Uint8*>(out->pixels) + dy * out->pitch);
        const int sy0 = static_cast<int>(srcY + dy * stepY);
        const int sy1 = std::max(
            sy0 + 1, std::min(img.height, static_cast<int>(srcY + (dy + 1) * stepY + 0.999)));
        for (int dx = 0; dx < w; ++dx) {
            const int sx0 = static_cast<int>(srcX + dx * stepX);
            const int sx1 = std::max(
                sx0 + 1, std::min(img.width, static_cast<int>(srcX + (dx + 1) * stepX + 0.999)));
            unsigned r = 0, g = 0, b = 0, count = 0;
            for (int sy = sy0; sy < sy1; ++sy) {
                const unsigned char* p = src + (static_cast<std::size_t>(sy) * img.width + sx0) * 4;
                for (int sx = sx0; sx < sx1; ++sx, p += 4, ++count) {
                    r += p[0];
                    g += p[1];
                    b += p[2];
                }
            }
            if (count == 0)
                count = 1;
            row[dx] = (static_cast<Uint32>(r / count)) | (static_cast<Uint32>(g / count) << 8) |
                      (static_cast<Uint32>(b / count) << 16) | 0xFF000000u;
        }
    }
    SDL_UnlockSurface(out);
    return out;
}

// Bilinear upscale of a small owned surface into a new w x h surface.
static SDL_Surface* upscaleBilinear(SDL_Surface* src, int w, int h)
{
    SDL_Surface* out =
        SDL_CreateRGBSurface(0, w, h, 32, 0x000000FF, 0x0000FF00, 0x00FF0000, 0xFF000000);
    if (!out)
        return nullptr;
    SDL_LockSurface(src);
    SDL_LockSurface(out);
    for (int y = 0; y < h; ++y) {
        const float fy = (y + 0.5f) * src->h / h - 0.5f;
        const int y0 = std::max(0, std::min(src->h - 1, static_cast<int>(std::floor(fy))));
        const int y1 = std::min(src->h - 1, y0 + 1);
        const float wy = std::max(0.0f, std::min(1.0f, fy - y0));
        Uint32* dst = reinterpret_cast<Uint32*>(static_cast<Uint8*>(out->pixels) + y * out->pitch);
        for (int x = 0; x < w; ++x) {
            const float fx = (x + 0.5f) * src->w / w - 0.5f;
            const int x0 = std::max(0, std::min(src->w - 1, static_cast<int>(std::floor(fx))));
            const int x1 = std::min(src->w - 1, x0 + 1);
            const float wx = std::max(0.0f, std::min(1.0f, fx - x0));
            auto px = [&](int sx, int sy) {
                return *reinterpret_cast<Uint32*>(static_cast<Uint8*>(src->pixels) +
                                                  sy * src->pitch + sx * 4);
            };
            const Uint32 p00 = px(x0, y0), p10 = px(x1, y0), p01 = px(x0, y1), p11 = px(x1, y1);
            Uint32 result = 0xFF000000u;
            for (int shift = 0; shift < 24; shift += 8) {
                const float a = ((p00 >> shift) & 0xFF) * (1 - wx) + ((p10 >> shift) & 0xFF) * wx;
                const float b = ((p01 >> shift) & 0xFF) * (1 - wx) + ((p11 >> shift) & 0xFF) * wx;
                result |= static_cast<Uint32>(a * (1 - wy) + b * wy) << shift;
            }
            dst[x] = result;
        }
    }
    SDL_UnlockSurface(out);
    SDL_UnlockSurface(src);
    return out;
}

SDL_Surface* artworkSurface(const DecodedImage& img, int w, int h)
{
    if (img.empty() || w <= 0 || h <= 0)
        return nullptr;
    const double ratio =
        (static_cast<double>(img.width) / img.height) / (static_cast<double>(w) / h);
    if (ratio >= 0.74 && ratio <= 1.35)
        return coverSurface(img, w, h); // close enough: a small crop beats any filler

    // Ambient fill: a tiny cover-crop scaled back up is a cheap, smooth blur.
    SDL_Surface* tiny = coverSurface(img, std::max(2, w / 10), std::max(2, h / 10));
    SDL_Surface* out = tiny ? upscaleBilinear(tiny, w, h) : nullptr;
    if (tiny)
        SDL_FreeSurface(tiny);
    if (!out)
        return coverSurface(img, w, h);
    blend(out, 0, 0, w, h, kCanvas, 120); // darken so the sharp copy stands out

    // Sharp copy, fully visible (contain), centred.
    int fw = w, fh = h;
    if (ratio > 1.0)
        fh = std::max(1, static_cast<int>(w / (static_cast<double>(img.width) / img.height)));
    else
        fw = std::max(1, static_cast<int>(h * (static_cast<double>(img.width) / img.height)));
    SDL_Surface* sharp = coverSurface(img, fw, fh);
    if (sharp) {
        SDL_Rect dst = {(w - fw) / 2, (h - fh) / 2, fw, fh};
        SDL_BlitSurface(sharp, nullptr, out, &dst);
        SDL_FreeSurface(sharp);
    }
    return out;
}

void roundCorners(SDL_Surface* fb, int x, int y, int w, int h, int radius, Rgb background)
{
    radius = std::min(radius, std::min(w, h) / 2);
    for (int row = 0; row < radius; ++row) {
        const int inset = cornerInset(row, radius);
        if (inset <= 0)
            continue;
        fill(fb, x, y + row, inset, 1, background);
        fill(fb, x + w - inset, y + row, inset, 1, background);
        fill(fb, x, y + h - 1 - row, inset, 1, background);
        fill(fb, x + w - inset, y + h - 1 - row, inset, 1, background);
    }
}

void bottomScrim(SDL_Surface* fb, int x, int y, int w, int h, int height, int maxAlpha)
{
    height = std::min(height, h);
    for (int i = 0; i < height; ++i) {
        const int t = (i + 1) * maxAlpha / height; // 0 at the top edge of the scrim
        blend(fb, x, y + h - height + i, w, 1, Rgb{0, 0, 0}, t);
    }
}

void placeholderTile(SDL_Surface* fb, int x, int y, int w, int h, const std::string& title,
                     int radius)
{
    // Curated, slightly desaturated gradient pairs that sit comfortably on the
    // navy canvas; the title picks one so a given item always looks the same.
    static const Rgb kTop[6] = {{34, 58, 112}, {28, 84, 96}, {70, 46, 108},
                                {96, 52, 70},  {44, 88, 70}, {88, 70, 40}};
    static const Rgb kBottom[6] = {{14, 22, 52}, {10, 36, 44}, {28, 18, 52},
                                   {44, 20, 30}, {16, 40, 30}, {40, 30, 14}};
    unsigned hash = 2166136261u;
    for (unsigned char ch : title)
        hash = (hash ^ ch) * 16777619u;
    const int pick = static_cast<int>(hash % 6);

    // Clip the gradient to the rounded shape by drawing it row by row.
    const int r = std::min(radius, std::min(w, h) / 2);
    for (int row = 0; row < h; ++row) {
        const int inset = rowInset(row, h, r);
        fill(fb, x + inset, y + row, w - 2 * inset, 1,
             mix(kTop[pick], kBottom[pick], h <= 1 ? 0 : row * 100 / (h - 1)));
    }

    // Monogram: first letter (and a second capital if the title has one).
    std::string mono;
    for (unsigned char ch : title) {
        if (std::isalnum(ch)) {
            mono.push_back(static_cast<char>(std::toupper(ch)));
            break;
        }
    }
    if (mono.empty())
        mono = "?";
    const int scale = std::min(w, h) >= 90 ? 3 : (std::min(w, h) >= 44 ? 2 : 1);
    const int tw = textWidth(mono, scale);
    const Rgb ink = mix(kTop[pick], Rgb{255, 255, 255}, 55);
    text(fb, x + (w - tw) / 2, y + (h - BitmapFont::GLYPH_H * scale) / 2, mono, ink, scale);
}

} // namespace ui
} // namespace miyoofin
