#ifndef MIYOOFIN_ON_SCREEN_KEYBOARD_HPP
#define MIYOOFIN_ON_SCREEN_KEYBOARD_HPP

#include "../input/Action.hpp"
#include <SDL2/SDL.h>
#include <string>
#include <vector>

namespace miyoofin {

/// Shared rectangular on-screen keyboard for all text-entry screens.
/// Uses a fixed 10-column grid so every row has identical outer width.
/// Rows 0–4 contain ten one-column keys; row 5 contains five two-column
/// action keys (SPACE, DEL, CLR, SUBMIT, CANCEL).
class OnScreenKeyboard {
public:
    struct Key {
        char ch;           // character to insert (0 for specials)
        const char *label; // display label
        int row;           // grid row (0–5)
        int col;           // starting grid column (0–9)
        int colSpan;       // 1 for normal keys, 2 for action keys
    };

    struct Config {
        const char *submitLabel = "[DONE]";
        int keyboardTop = 104;
    };

    OnScreenKeyboard(); // default config
    explicit OnScreenKeyboard(const Config &config);

    /// Reset to initial state (lowercase, home position).
    void reset();

    /// True when alphabetic keys are uppercased.
    bool capsEnabled() const;

    /// Current keyboard-top y coordinate.
    int keyboardTop() const;

    /// Y coordinate just below the last key row.
    int keyboardBottom() const;

    // ── navigation ──────────────────────────────────────────────────

    /// Consume a logical action and return:
    ///   -1  – not consumed by the keyboard
    ///    0  – consumed (navigation / caps toggle; no character)
    ///  > 0  – a character code the caller should insert or dispatch
    int handleAction(Action action);

    /// Currently highlighted key (never null after construction).
    const Key *activeKey() const;

    /// True if activeKey() points to a valid key.
    bool selectionValid() const;

    /// Row index of the currently highlighted key.
    int selectionRow() const;

    // ── character processing ────────────────────────────────────────

    /// Return the character that `key` represents, applying caps.
    char processKey(const Key &key) const;

    // ── special character codes (screen dispatches these) ───────────
    static constexpr char KEY_DEL    = '\b';
    static constexpr char KEY_CLR    = 0x7F;
    static constexpr char KEY_SUBMIT = 0x01;
    static constexpr char KEY_CANCEL = 0x02;

    // ── layout constants ────────────────────────────────────────────
    static constexpr int GRID_COLS     = 10;
    static constexpr int NUM_ROWS      = 6;
    static constexpr int KEY_W         = 59;
    static constexpr int KEY_H         = 44;
    static constexpr int KEY_GAP       = 3;
    static constexpr int KEY_LABEL_SCALE = 2;
    static constexpr int KEYBOARD_WIDTH = GRID_COLS * KEY_W + (GRID_COLS - 1) * KEY_GAP; // 617
    static constexpr int KEYBOARD_X     = (640 - KEYBOARD_WIDTH) / 2;                    // 11

    // ── rendering ───────────────────────────────────────────────────

    /// Draw the full keyboard onto the software framebuffer.
    void render(SDL_Surface *fb) const;

    // ── label helpers (public for testing) ──────────────────────────

    /// Display label for a key, applying caps to single letters.
    std::string keyLabel(const Key &key) const;

private:
    Config m_config;
    std::vector<Key> m_keys;
    int m_activeKeyRow = 0;
    int m_activeKeyCol = 0;
    bool m_caps = false;

    void buildKeyboard();
    int keyIndex(int row, int col) const;

    bool navigateUp();
    bool navigateDown();
    bool navigateLeft();
    bool navigateRight();
};

} // namespace miyoofin

#endif // MIYOOFIN_ON_SCREEN_KEYBOARD_HPP
