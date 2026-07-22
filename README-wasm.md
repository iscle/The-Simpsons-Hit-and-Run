# Browser / WebAssembly build (Emscripten)

▶ **Playable build:** https://iscle.github.io/the-simpsons-hit-and-run-wasm/
(prebuilt engine, hosted on GitHub Pages —
[repo](https://github.com/iscle/the-simpsons-hit-and-run-wasm)). You supply your
own game data via `?assets=<url>`; see that repo's README.

This port can be compiled to WebAssembly and run in a browser using
[Emscripten](https://emscripten.org/). The browser build uses the GLES2
renderer (WebGL), Emscripten's SDL2 and libpng ports, its built-in OpenAL
implementation, and pthreads (Web Workers + SharedArrayBuffer).

## Prerequisites

- Emscripten 3.1.50+ (tested with 6.0.x), e.g. `brew install emscripten`
- CMake 3.10+
- The PC game assets (see below)

## Building

```sh
emcmake cmake -B build-wasm \
    -DCMAKE_BUILD_TYPE=Release \
    -DSRR2_P3D_PDDI=GLES2 \
    -DSRR2_BUILD_TESTS=OFF
cmake --build build-wasm -j
```

This produces `build-wasm/code/SRR2.html`, `SRR2.js` and `SRR2.wasm`.

## Game assets

As with the Switch/Vita port, the PC release assets are required (`art/`,
`sound/`, `scripts/`, `music/`, `CREDITS/`, `cars/`, ... from the game's
install directory). Do not use assets from the source leak.

Assets are NOT bundled into the build. The page streams them on demand from
the server under `/assets/` using HTTP Range requests (see below), so only
the file regions the game actually reads are fetched and kept in memory
(a small per-file LRU of 1 MB chunks). This keeps tab memory low and makes
the first load near-instant. The `movies` folder is never requested (movie
playback is stubbed out — FMVs finish instantly).

## Running

Browsers only enable SharedArrayBuffer (required for pthreads) on
cross-origin-isolated pages, so the files must be served with COOP/COEP
headers. A ready-made dev server is included:

```sh
python3 serve-wasm.py 8080 build-wasm/code /path/to/simpsons-pc-assets
# then open http://localhost:8080/SRR2.html
```

The third argument mounts the PC asset directory at `/assets/` with HTTP
Range support. For production hosting, serve the asset directory the same
way (any static host that supports `Range` requests works — most do) and
keep the same `/assets/<path>` URL layout. Note that asset paths are
requested with the same case as on disk, so use a case-preserving copy on
case-sensitive hosts.

For production hosting, send these headers with every response:

```
Cross-Origin-Opener-Policy: same-origin
Cross-Origin-Embedder-Policy: require-corp
```

## Controls (browser build)

The engine only reads game controllers, so the browser build registers a
keyboard-backed virtual gamepad:

| Key | Gamepad | In menus / game |
|---|---|---|
| Arrow keys | D-pad | navigate |
| WASD | Left stick | move |
| Enter / Space / Z | A | accept / jump |
| Esc | Start + B | pause / back |
| X | B | back / action |
| C / **Left&nbsp;click** / F1 | X | attack / "disable tutorials" |
| V / **Right&nbsp;click** | Y | |
| Q / E | Shoulder L / R | |
| Shift / Ctrl | Trigger L / R | run |
| Tab / Backspace | Select/Back | |

A real gamepad (Gamepad API) also works if the browser exposes it as an SDL
game controller.

## Browser-build limitations

- **Movies**: FMV playback is stubbed (`RAD_MOVIEPLAYER_NULL`); intro/mission
  movies complete immediately instead of playing.
- **Audio streaming**: `AL_SOFT_map_buffer` is not available in Emscripten's
  OpenAL, so streamed sounds (music, streamed dialog) are emulated with a
  queue-based path (`alSourceQueueBuffers`): each region the engine writes
  into its ring buffer is queued as its own AL buffer. Brief underruns can
  cause small audio gaps under heavy load.
- **Reverb**: EFX is unavailable in browsers, so environmental reverb is
  disabled (same as any OpenAL implementation without `ALC_EXT_EFX`).
- **Saves**: the in-browser filesystem is in-memory; saved games do not
  persist across page reloads.
- **Quitting**: exiting the game stops the main loop but does not tear the
  page down; reload the page to restart.
