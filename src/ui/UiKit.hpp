#ifndef MIYOOFIN_UI_KIT_HPP
#define MIYOOFIN_UI_KIT_HPP

#include "../image/ImageDecoder.hpp"
#include "Design.hpp"
#include <SDL2/SDL.h>
#include <string>
#include <vector>

namespace miyoofin {
namespace ui {

using design::Rgb;

// ---------------------------------------------------------------- primitives
void fill(SDL_Surface* fb, int x, int y, int w, int h, Rgb c);
// Alpha-blended fill (alpha 0..255); the framebuffer is 32-bit.
void blend(SDL_Surface* fb, int x, int y, int w, int h, Rgb c, int alpha);
void outline(SDL_Surface* fb, int x, int y, int w, int h, Rgb c);
// Fills with true rounded corners (radius <= 8 reads best at 640x480).
void roundFill(SDL_Surface* fb, int x, int y, int w, int h, int radius, Rgb c);
// 1px rounded outline.
void roundOutline(SDL_Surface* fb, int x, int y, int w, int h, int radius, Rgb c);
void gradientV(SDL_Surface* fb, int x, int y, int w, int h, Rgb top, Rgb bottom);
Rgb mix(Rgb a, Rgb b, int percentOfB);

// ---------------------------------------------------------------------- text
// Transparent-background text (8x16 font, `scale` enlarges it). Returns width.
int textWidth(const std::string& s, int scale = 1);
int text(SDL_Surface* fb, int x, int y, const std::string& s, Rgb c, int scale = 1);
// Text with a 1px dark shadow, for legibility over artwork.
int textShadow(SDL_Surface* fb, int x, int y, const std::string& s, Rgb c, int scale = 1);
// Truncates with ".." so the text fits `maxWidthPx`.
std::string fit(const std::string& s, int maxWidthPx, int scale = 1);
int textClamped(SDL_Surface* fb, int x, int y, int maxWidthPx, const std::string& s, Rgb c,
                int scale = 1);
// Word-wraps to `maxWidthPx`; the last allowed line is truncated with "..".
std::vector<std::string> wrap(const std::string& s, int maxWidthPx, int maxLines, int scale = 1);

// ------------------------------------------------------------------ controls
// Soft electric-blue focus ring drawn just outside (x, y, w, h).
void focusRing(SDL_Surface* fb, int x, int y, int w, int h, int radius = design::kRadius);

enum class ButtonStyle
{
    Primary,   // filled accent
    Secondary, // raised surface
    Danger     // red outline / fill when focused
};
void button(SDL_Surface* fb, int x, int y, int w, int h, const std::string& label,
            ButtonStyle style, bool focused, bool enabled = true);

// Small rounded label (year, runtime, state). Returns its width.
int chip(SDL_Surface* fb, int x, int y, const std::string& label, Rgb bg, Rgb fg);

void progressBar(SDL_Surface* fb, int x, int y, int w, int h, int percent,
                 Rgb fillColor = design::kAccent);

// A card-like panel: rounded surface with a hairline border.
void panel(SDL_Surface* fb, int x, int y, int w, int h, Rgb surface = design::kPanel,
           Rgb border = design::kBorder);

// Single-line text field (focused fields get the accent outline).
void field(SDL_Surface* fb, int x, int y, int w, int h, const std::string& label,
           const std::string& value, bool focused);

// Inline text input for the keyboard screens: muted label on the left, the
// value (its tail, when too long) on the right and a caret on the focused
// field. `masked` shows asterisks (passwords).
void inputField(SDL_Surface* fb, int x, int y, int w, int h, const std::string& label,
                const std::string& value, bool focused, bool masked, int labelWidthPx);

// --------------------------------------------------------------------- icons
void iconBattery(SDL_Surface* fb, int x, int y, int percent, bool charging);
void iconPlay(SDL_Surface* fb, int x, int y, int size, Rgb c);
void iconDownload(SDL_Surface* fb, int x, int y, int size, Rgb c);
void iconCheck(SDL_Surface* fb, int x, int y, int size, Rgb c);
void iconChevron(SDL_Surface* fb, int x, int y, int size, bool pointRight, Rgb c);
void statusDot(SDL_Surface* fb, int x, int y, int size, Rgb c);

// -------------------------------------------------------------------- chrome
enum class Key
{
    Dpad,
    A,
    B,
    X,
    Y,
    L,
    R,
    LR,
    L2,
    Menu,
    Start,
    Select
};
struct Hint
{
    Key key;
    std::string label;
};
// Draws one controller-button badge; returns its width.
int keyBadge(SDL_Surface* fb, int x, int y, Key key);

struct HeaderSpec
{
    std::vector<std::string> tabs; // empty on drill-down pages
    int activeTab = 0;
    std::string title; // breadcrumb/title when there are no tabs
    int batteryPercent = -1;
    bool charging = false;
    bool showStatus = true;       // battery + clock cluster
    int tabPad = -1, tabGap = -1; // pill padding / gap override (-1 = the shared defaults)
};
// Header strip (design::kHeaderH tall): wordmark, tabs or title, status cluster.
void header(SDL_Surface* fb, const HeaderSpec& spec);

enum class LinkState
{
    Unknown,
    Connected,
    Offline,
    Checking
};
struct FooterSpec
{
    std::vector<Hint> hints;
    // A prompt that replaces the hints (e.g. "Press Y again to log out").
    std::string message;
    Rgb messageColor = design::kWarning;
    // A short status drawn before the link text (e.g. sync progress).
    std::string note;
    Rgb noteColor = design::kAccent;
    std::string rightText; // e.g. "seggie connected"
    LinkState link = LinkState::Unknown;
    bool showLink = true;
};
// Footer strip (design::kFooterH tall): control hints left, link status right.
void footer(SDL_Surface* fb, const FooterSpec& spec);

// Full-screen status page (startup, connecting, signing in, welcome): large
// wordmark, a headline, up to two wrapped detail lines, an optional address
// chip and animated dots while `busy`. The footer shows `hints` only.
struct SplashSpec
{
    std::string headline;
    Rgb headlineColor = design::kText;
    std::string detail;
    std::string address;
    bool busy = false;
    std::vector<Hint> hints;
};
void splash(SDL_Surface* fb, const SplashSpec& spec);

// ------------------------------------------------------------------- artwork
// Scales `img` to exactly w x h, CROPPING the overflow (cover, centred) with a
// box filter, returning an owned SDL surface (caller frees), or nullptr.
SDL_Surface* coverSurface(const DecodedImage& img, int w, int h);
// Artwork sized exactly w x h that never shows letterbox bars: covers when the
// image shape is close to the box (within ~35%), otherwise fills with a blurred,
// darkened copy of the image and centres a sharp, fully visible copy on top.
// Returns an owned surface (caller frees), or nullptr.
SDL_Surface* artworkSurface(const DecodedImage& img, int w, int h);
// Repaints the four corners outside a rounded rectangle with `background`, so
// an already-blitted rectangular image reads as rounded.
void roundCorners(SDL_Surface* fb, int x, int y, int w, int h, int radius, Rgb background);
// Darkens the bottom `height` pixels with a smooth 0 -> maxAlpha gradient, so
// text or bars drawn over artwork stay legible.
void bottomScrim(SDL_Surface* fb, int x, int y, int w, int h, int height, int maxAlpha);
// Styled tile (title-seeded gradient + monogram) for items without artwork.
void placeholderTile(SDL_Surface* fb, int x, int y, int w, int h, const std::string& title,
                     int radius = design::kRadius);

} // namespace ui
} // namespace miyoofin

#endif // MIYOOFIN_UI_KIT_HPP
