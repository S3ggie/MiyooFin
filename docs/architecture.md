# MiyooFin Architecture

## Overview

MiyooFin is a C++17 SDL2 application using a single-software-framebuffer
rendering approach. All drawing is done onto one 640x480 RGBA32 surface
which is uploaded to a streaming texture and presented each frame.

## Rendering

- One `SDL_Surface` (640x480, RGBA32) — the software framebuffer
- One `SDL_Texture` (STREAMING) — uploaded from the surface each frame
- No `SDL_TEXTUREACCESS_TARGET` (not supported on Miyoo SDL2)
- No `SDL_CreateTextureFromSurface` for UI elements
- Bitmap font (8x16) embedded as pixel data — no SDL2_ttf dependency

## Screen System

- `Screen` abstract interface: enter/leave/handleAction/update/render
- `ScreenStack` manages push/pop navigation
- Only the top screen receives events and renders

## Input

- `InputManager` polls SDL events and converts to logical `Action` values
- Raw events are logged for the diagnostics screen
- Key mapping is tentative and will be confirmed on-device

## Directory Structure

```
src/
  main.cpp            Entry point
  app/                App, Screen, ScreenStack
  input/              InputManager, Action enum
  ui/                 Theme, BitmapFont, FocusGrid, screens/
  net/                HttpClient, Url, DeviceIdentity, WorkQueue
  api/                JellyfinApi, ApiTypes
  model/              DTOs + JSON parsing
  cache/              ImageCache, CacheKey
  download/           DownloadManager (Milestone 2)
  settings/           SettingsStore
  log/                Logger
  time/               Timer
include/miyoofin/     Public headers (version, client identity)
tests/                Unit tests
assets/               Fonts, placeholder art, icons
distributions/        OnionOS packaging
docs/                 Documentation, pinned OpenAPI spec
output/               Build artifacts (gitignored)
```

## Dependency Graph (Checkpoint A)

```
main.cpp
  └─ App.hpp
       ├─ ScreenStack.hpp
       │    └─ Screen.hpp
       └─ InputManager.hpp
            └─ Action.hpp
       └─ screens/
            ├─ StartupScreen.hpp
            └─ InputDiagnosticsScreen.hpp
       └─ Theme.hpp
       └─ BitmapFont.hpp
```