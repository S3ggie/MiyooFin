// MiyooFin headless UI harness: scripted-input driver.
//
// LD_PRELOAD shim that interposes SDL_PollEvent and replays a scripted
// key-event timeline against the running desktop build. See
// docs/ui-script-harness.md for the rationale (chosen over xdotool),
// the script format, and usage.
//
// Test-only tooling: built to output/ui-script/shim.so, never linked into
// the app and never shipped. Configuration is env-only:
//   MIYOOFIN_UI_SCRIPT    path to the script file (required)
//   MIYOOFIN_UI_LOG       app stdout log to poll for WAIT_LOG (required)
//   MIYOOFIN_UI_SHOT_DIR  directory for SCREENSHOT BMPs (required)
//   MIYOOFIN_UI_RESULT    result file; FAIL lines appended here (required)
//
// Script directives (one per line, '#' comments, blanks ignored):
//   WAIT_LOG <substring> [timeout_ms]  poll the app log until it appears
//   KEY <name> [hold_ms] [settle_ms]    press+release a mapped key
//   SCREENSHOT <name>                   framebuffer BMP into the shot dir
//   SETTLE <ms>                         bounded wait for marker-less workers
//   QUIT                                inject SDL_QUIT so the app exits 0
//
// The shim drives the DESKTOP input mapping (MIYOOFIN_DESKTOP_INPUT=1)
// by default: WASD d-pad, Enter confirm, Backspace back, T/R-tab, E/L-tab.
// For the device (ARM) variant, setting MIYOOFIN_UI_DEVICE_KEYS=1 remaps
// the same key names to the Miyoo physical scancodes (see table below),
// so the same script file can be reused unchanged. Either mode also
// accepts the Raw:<scancode> escape (e.g. KEY Raw:79) to inject one
// device scancode directly, for buttons with no named key.
//
//   Name      Desktop         Device (scancode)
//   Up        W               82 (d-pad)
//   Down      S               81 (d-pad)
//   Left      A               80 (d-pad)
//   Right     D               79 (d-pad)
//   Confirm   Enter           44 (A button)
//   Back      Backspace       224 (B button)
//   NextTab   T               23 (R shoulder)
//   PrevTab   E               8 (L shoulder)
// Other device buttons (START=40, MENU=41, R2=42, L2=43, X=225, Y=226,
// SELECT=228) are reachable only via Raw:<scancode>.

#include <SDL2/SDL.h>

#include <dlfcn.h>

#include <cctype>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

namespace {

typedef int (*PollEventFn)(SDL_Event*);

PollEventFn realPollEvent()
{
    static PollEventFn fn = nullptr;
    if (!fn) {
        fn = reinterpret_cast<PollEventFn>(dlsym(RTLD_NEXT, "SDL_PollEvent"));
        if (!fn) {
            std::fprintf(stderr, "[uishim] FATAL: dlsym(SDL_PollEvent) failed\n");
        }
    }
    return fn;
}

uint64_t nowMs()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000u + static_cast<uint64_t>(ts.tv_nsec) / 1000000u;
}

std::string trim(const std::string& s)
{
    size_t b = 0;
    while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b])))
        ++b;
    size_t e = s.size();
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1])))
        --e;
    return s.substr(b, e - b);
}

struct KeyMap
{
    const char* name;
    SDL_Scancode sc;
    SDL_Keycode sym;
};

struct Step
{
    enum Kind
    {
        WaitLog,
        Key,
        Screenshot,
        Quit,
        Settle
    } kind = Quit;
    std::string arg;            // WaitLog substring / Screenshot name
    std::string keyName;        // Key step (as written in the script)
    KeyMap key = {};            // Key step (resolved scancode/sym)
    uint64_t holdMs = 80;       // Key step
    uint64_t settleMs = 400;    // Key step
    uint64_t timeoutMs = 20000; // WaitLog step
    uint64_t settleTotalMs = 0; // Settle step
};

const KeyMap kKeys[] = {
    {"Up", SDL_SCANCODE_W, SDLK_w},
    {"Down", SDL_SCANCODE_S, SDLK_s},
    {"Left", SDL_SCANCODE_A, SDLK_a},
    {"Right", SDL_SCANCODE_D, SDLK_d},
    {"Confirm", SDL_SCANCODE_RETURN, SDLK_RETURN},
    {"Back", SDL_SCANCODE_BACKSPACE, SDLK_BACKSPACE},
    {"NextTab", SDL_SCANCODE_T, SDLK_t},
    {"PrevTab", SDL_SCANCODE_E, SDLK_e},
};

// Device (Miyoo Mini Plus) mapping: same names, physical scancodes from
// InputManager (d-pad 79-82, A=44, B=224, R=23, L=8). Sym is unknown on
// device — the app maps by scancode — so SDLK_UNKNOWN throughout.
const KeyMap kDeviceKeys[] = {
    {"Up", static_cast<SDL_Scancode>(82), SDLK_UNKNOWN},
    {"Down", static_cast<SDL_Scancode>(81), SDLK_UNKNOWN},
    {"Left", static_cast<SDL_Scancode>(80), SDLK_UNKNOWN},
    {"Right", static_cast<SDL_Scancode>(79), SDLK_UNKNOWN},
    {"Confirm", static_cast<SDL_Scancode>(44), SDLK_UNKNOWN},
    {"Back", static_cast<SDL_Scancode>(224), SDLK_UNKNOWN},
    {"NextTab", static_cast<SDL_Scancode>(23), SDLK_UNKNOWN},
    {"PrevTab", static_cast<SDL_Scancode>(8), SDLK_UNKNOWN},
};

bool resolveKey(const std::string& name, bool deviceKeys, KeyMap& out)
{
    // Raw device scancode escape: KEY Raw:79 injects that SDL scancode
    // directly (either mapping mode). Range-guarded; SDL scancodes fit.
    if (name.compare(0, 4, "Raw:") == 0) {
        const char* num = name.c_str() + 4;
        if (!*num)
            return false;
        char* end = nullptr;
        const long v = std::strtol(num, &end, 10);
        if (end == num || *end != '\0' || v < 0 || v > 512)
            return false;
        out.name = "Raw";
        out.sc = static_cast<SDL_Scancode>(v);
        out.sym = SDLK_UNKNOWN;
        return true;
    }
    const KeyMap* table = deviceKeys ? kDeviceKeys : kKeys;
    const size_t n = deviceKeys ? sizeof(kDeviceKeys) / sizeof(kDeviceKeys[0])
                                : sizeof(kKeys) / sizeof(kKeys[0]);
    for (size_t i = 0; i < n; ++i) {
        if (name == table[i].name) {
            out = table[i];
            return true;
        }
    }
    return false;
}

struct Harness
{
    bool loaded = false;
    bool loadFailed = false;
    std::string loadError;
    std::vector<Step> steps;
    size_t stepIdx = 0;
    // Per-step runtime state.
    uint64_t stepStartMs = 0;
    bool stepStarted = false;
    bool keyDownSent = false;
    uint64_t keyUpAtMs = 0;
    uint64_t stepDoneAtMs = 0;
    bool quitSent = false;
    bool quitAppended = false;
    std::vector<SDL_Event> pending;
    std::string logPath;
    std::string shotDir;
    std::string resultPath;
    bool failed = false;
    // MIYOOFIN_UI_DEVICE_KEYS=1: KEY names resolve to Miyoo scancodes.
    bool deviceKeys = false;

    void fail(const std::string& msg)
    {
        if (failed)
            return; // keep the first failure only
        failed = true;
        std::fprintf(stderr, "[uishim] FAIL: %s\n", msg.c_str());
        if (!resultPath.empty()) {
            if (FILE* f = std::fopen(resultPath.c_str(), "a")) {
                std::fprintf(f, "FAIL %s\n", msg.c_str());
                std::fclose(f);
            }
        }
    }

    void pass(const std::string& msg)
    {
        std::fprintf(stderr, "[uishim] %s\n", msg.c_str());
        if (!resultPath.empty()) {
            if (FILE* f = std::fopen(resultPath.c_str(), "a")) {
                std::fprintf(f, "PASS %s\n", msg.c_str());
                std::fclose(f);
            }
        }
    }

    void load()
    {
        if (loaded || loadFailed)
            return;
        loaded = true;
        const char* script = std::getenv("MIYOOFIN_UI_SCRIPT");
        const char* log = std::getenv("MIYOOFIN_UI_LOG");
        const char* shots = std::getenv("MIYOOFIN_UI_SHOT_DIR");
        const char* result = std::getenv("MIYOOFIN_UI_RESULT");
        if (!script || !*script) {
            loadFailed = true;
            loadError = "MIYOOFIN_UI_SCRIPT unset";
            return;
        }
        if (!log || !*log) {
            loadFailed = true;
            loadError = "MIYOOFIN_UI_LOG unset";
            return;
        }
        if (!shots || !*shots) {
            loadFailed = true;
            loadError = "MIYOOFIN_UI_SHOT_DIR unset";
            return;
        }
        if (!result || !*result) {
            loadFailed = true;
            loadError = "MIYOOFIN_UI_RESULT unset";
            return;
        }
        logPath = log;
        shotDir = shots;
        resultPath = result;
        // Same truthiness rule as MIYOOFIN_DESKTOP_INPUT: set and not '0'.
        const char* dk = std::getenv("MIYOOFIN_UI_DEVICE_KEYS");
        deviceKeys = dk && dk[0] != '\0' && dk[0] != '0';
        FILE* f = std::fopen(script, "r");
        if (!f) {
            loadFailed = true;
            loadError = std::string("cannot open script: ") + script;
            return;
        }
        char buf[1024];
        unsigned lineNo = 0;
        while (std::fgets(buf, sizeof(buf), f)) {
            ++lineNo;
            std::string line = trim(buf);
            if (line.empty() || line[0] == '#')
                continue;
            char cmd[32] = {0}, a1[256] = {0}, a2[64] = {0}, a3[64] = {0};
            int n = std::sscanf(line.c_str(), "%31s %255s %63s %63s", cmd, a1, a2, a3);
            if (n < 1)
                continue;
            Step st;
            if (std::strcmp(cmd, "WAIT_LOG") == 0) {
                if (n < 2) {
                    loadFailed = true;
                    loadError = "WAIT_LOG needs a substring";
                    break;
                }
                // Substring is the rest of the line after the command so it
                // may contain spaces (e.g. "[HomeScreen] Library loaded").
                std::string rest = trim(line.substr(std::strlen(cmd)));
                // Optional trailing timeout: last token all digits.
                st.kind = Step::WaitLog;
                st.timeoutMs = 20000;
                const size_t sp = rest.find_last_of(" \t");
                if (sp != std::string::npos) {
                    const std::string tail = rest.substr(sp + 1);
                    bool digits = !tail.empty();
                    for (char c : tail)
                        digits = digits && std::isdigit(static_cast<unsigned char>(c));
                    if (digits) {
                        st.timeoutMs =
                            static_cast<uint64_t>(std::strtoull(tail.c_str(), nullptr, 10));
                        rest = trim(rest.substr(0, sp));
                    }
                }
                st.arg = rest;
            } else if (std::strcmp(cmd, "KEY") == 0) {
                if (n < 2) {
                    loadFailed = true;
                    loadError = std::string("bad KEY step at line ") + std::to_string(lineNo);
                    break;
                }
                st.kind = Step::Key;
                st.keyName = a1;
                if (!resolveKey(a1, deviceKeys, st.key)) {
                    loadFailed = true;
                    loadError = std::string("bad KEY step at line ") + std::to_string(lineNo);
                    break;
                }
                if (n >= 3)
                    st.holdMs = static_cast<uint64_t>(std::strtoull(a2, nullptr, 10));
                if (n >= 4)
                    st.settleMs = static_cast<uint64_t>(std::strtoull(a3, nullptr, 10));
            } else if (std::strcmp(cmd, "SCREENSHOT") == 0) {
                if (n < 2) {
                    loadFailed = true;
                    loadError = "SCREENSHOT needs a name";
                    break;
                }
                st.kind = Step::Screenshot;
                st.arg = a1;
            } else if (std::strcmp(cmd, "QUIT") == 0) {
                st.kind = Step::Quit;
            } else if (std::strcmp(cmd, "SETTLE") == 0) {
                // Bounded dumb wait for async worker completion that emits
                // no pollable marker. Use only where WAIT_LOG cannot apply;
                // the screenshot assertion after it is the real verdict.
                if (n < 2) {
                    loadFailed = true;
                    loadError = "SETTLE needs ms";
                    break;
                }
                st.kind = Step::Settle;
                st.settleTotalMs = static_cast<uint64_t>(std::strtoull(a1, nullptr, 10));
            } else {
                loadFailed = true;
                loadError = std::string("unknown command at line ") + std::to_string(lineNo);
                break;
            }
            steps.push_back(st);
        }
        std::fclose(f);
        if (!loadFailed) {
            std::fprintf(stderr, "[uishim] loaded %zu steps from %s (%s keys)\n", steps.size(),
                         script, deviceKeys ? "device" : "desktop");
        }
    }

    bool logContains(const std::string& needle)
    {
        FILE* f = std::fopen(logPath.c_str(), "r");
        if (!f)
            return false;
        char buf[4096];
        std::string tail;
        // Bounded scan: only the tail matters, keep memory flat.
        std::string window;
        while (std::fgets(buf, sizeof(buf), f)) {
            window += buf;
            if (window.size() > 1u << 20)
                window.erase(0, window.size() - (1u << 20));
        }
        std::fclose(f);
        tail = window;
        return tail.find(needle) != std::string::npos;
    }

    void pushKey(const KeyMap& k, bool down)
    {
        SDL_Event ev;
        std::memset(&ev, 0, sizeof(ev));
        ev.type = down ? SDL_KEYDOWN : SDL_KEYUP;
        ev.key.type = ev.type;
        ev.key.timestamp = 0;
        ev.key.windowID = 0;
        ev.key.state = down ? SDL_PRESSED : SDL_RELEASED;
        ev.key.repeat = 0;
        ev.key.keysym.scancode = k.sc;
        ev.key.keysym.sym = k.sym;
        ev.key.keysym.mod = KMOD_NONE;
        pending.push_back(ev);
        std::fprintf(stderr, "[uishim] KEY %s %s\n", k.name, down ? "down" : "up");
    }

    // Advance the script; returns true when a synthetic event was queued.
    void pump()
    {
        load();
        if (loadFailed)
            return;
        if (quitSent)
            return; // QUIT is one-shot: the app is on its way out.
        if (failed && !quitAppended) {
            // After a failure, fast-forward to QUIT so the app still exits
            // cleanly and the runner can report. One-shot: never append
            // repeatedly, or poll()'s while-drain spins forever.
            quitAppended = true;
            for (size_t i = stepIdx; i < steps.size(); ++i) {
                if (steps[i].kind == Step::Quit) {
                    stepIdx = i;
                    stepStarted = false;
                    break;
                }
            }
            if (stepIdx >= steps.size() || steps[stepIdx].kind != Step::Quit) {
                Step q;
                q.kind = Step::Quit;
                steps.push_back(q);
                stepIdx = steps.size() - 1;
                stepStarted = false;
            }
        }
        if (stepIdx >= steps.size() || !pending.empty())
            return;
        const uint64_t now = nowMs();
        Step& st = steps[stepIdx];
        if (!stepStarted) {
            stepStarted = true;
            stepStartMs = now;
            keyDownSent = false;
            if (st.kind == Step::Key) {
                keyUpAtMs = now + st.holdMs;
                stepDoneAtMs = keyUpAtMs + st.settleMs;
            } else if (st.kind == Step::Screenshot) {
                // Point the app's screenshot hook at a fresh per-shot path,
                // then raise the flag file the app polls once per frame.
                const std::string dest = shotDir + "/" + st.arg + ".bmp";
                std::remove(dest.c_str());
                const int se = setenv("MIYOOFIN_SCREENSHOT_PATH", dest.c_str(), 1);
                const char* back = std::getenv("MIYOOFIN_SCREENSHOT_PATH");
                FILE* f = std::fopen("/tmp/miyoofin-screenshot-request", "w");
                const bool flagged = (f != nullptr);
                if (f)
                    std::fclose(f);
                std::fprintf(
                    stderr, "[uishim] SCREENSHOT %s dest=%s setenv=%d readback=%s flag=%d\n",
                    st.arg.c_str(), dest.c_str(), se, back ? back : "(null)", flagged ? 1 : 0);
            } else if (st.kind == Step::Quit) {
                SDL_Event ev;
                std::memset(&ev, 0, sizeof(ev));
                ev.type = SDL_QUIT;
                pending.push_back(ev);
                quitSent = true;
                pass("script complete, quitting app");
                ++stepIdx;
                stepStarted = false;
                return;
            }
        }
        switch (st.kind) {
        case Step::Key: {
            if (!keyDownSent) {
                pushKey(st.key, true);
                keyDownSent = true;
                return;
            }
            if (now >= keyUpAtMs && keyDownSent) {
                // Send KEYUP exactly once: mark via keyUpAtMs sentinel.
                keyUpAtMs = UINT64_MAX;
                pushKey(st.key, false);
                return;
            }
            if (now >= stepDoneAtMs) {
                ++stepIdx;
                stepStarted = false;
            }
            break;
        }
        case Step::WaitLog: {
            if (logContains(st.arg)) {
                pass(std::string("WAIT_LOG matched: ") + st.arg);
                ++stepIdx;
                stepStarted = false;
            } else if (now - stepStartMs >= st.timeoutMs) {
                fail("WAIT_LOG timeout (" + std::to_string(st.timeoutMs) + "ms): " + st.arg);
            }
            break;
        }
        case Step::Screenshot: {
            const std::string dest = shotDir + "/" + st.arg + ".bmp";
            FILE* f = std::fopen(dest.c_str(), "rb");
            if (f) {
                std::fclose(f);
                pass(std::string("captured ") + dest);
                ++stepIdx;
                stepStarted = false;
            } else if (now - stepStartMs >= 5000) {
                const char* devDefault = "/mnt/SDCARD/App/MiyooFin/screenshot.bmp";
                FILE* ad = std::fopen(dest.c_str(), "rb");
                FILE* bd = std::fopen(devDefault, "rb");
                std::fprintf(stderr,
                             "[uishim] SCREENSHOT miss: dest_exists=%d dev_default_exists=%d\n",
                             ad ? 1 : 0, bd ? 1 : 0);
                if (ad)
                    std::fclose(ad);
                if (bd)
                    std::fclose(bd);
                fail("SCREENSHOT timeout: " + dest);
            }
            break;
        }
        case Step::Settle: {
            if (now - stepStartMs >= st.settleTotalMs) {
                ++stepIdx;
                stepStarted = false;
            }
            break;
        }
        case Step::Quit:
            break;
        }
    }
};

Harness& harness()
{
    static Harness h;
    return h;
}

} // namespace

extern "C" int SDL_PollEvent(SDL_Event* event)
{
    Harness& h = harness();
    h.pump();
    if (!h.pending.empty()) {
        *event = h.pending.front();
        h.pending.erase(h.pending.begin());
        return 1;
    }
    PollEventFn real = realPollEvent();
    if (!real)
        return 0;
    return real(event);
}
