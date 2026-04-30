# Godot PCK Tool — Browser Edition

A fully client-side web interface for working with Godot `.pck` archive files.
No server, no installation, no data leaves your machine.

## Workflow

**Extract → edit files locally → Pack**

1. Use **Extract** to download all files from a `.pck` as a `.zip`.
2. Open the `.zip` and edit or replace files using your file manager.
3. Use **Pack** to turn the modified `.zip` back into a `.pck`.

## Modes

### List
Browse the contents of a `.pck` — file names, folder structure, and sizes. Nothing is downloaded.

### Extract
Extract all files from a `.pck` into a `.zip` archive you can open and edit locally.
The original directory structure is preserved.
Result downloads as `<name>.zip`.

### Pack
Create a `.pck` from a `.zip` archive.
Every file in the ZIP becomes a file in the PCK at the same path.
Result downloads as `<name>.pck`.

---

## Building

Requires [Emscripten](https://emscripten.org/) and CMake. The easiest way to get
all dependencies is via the Nix devShell at the repository root:

```sh
nix develop          # drops you into a shell with emscripten, cmake, ninja
make install-wasm    # compiles and copies godotpcktool.js + .wasm into web/
```

Without Nix, with Emscripten installed and activated (`source emsdk_env.sh`):

```sh
emcmake cmake -B build-wasm web/
cmake --build build-wasm --parallel
cp build-wasm/godotpcktool.js build-wasm/godotpcktool.wasm web/
```

---

## Serving

After building, serve the `web/` directory with any static file server.
The page **cannot** be opened directly as a `file://` URL — browsers block
WASM fetches from local filesystem URLs.

```sh
# Python (built-in)
python3 -m http.server 8080 --directory web/

# Node.js
npx serve web/
```

Then open `http://localhost:8080` in your browser.

---

## Browser compatibility

Requires a modern browser with WebAssembly and ES2017 support:

- Chrome / Edge 57+
- Firefox 52+
- Safari 11+

---

## Limitations

- **Encrypted PCKs** are not supported — extraction and listing will fail.
- **No progress bar** for large archives — the page is unresponsive during
  processing; this is normal.
- **Godot version metadata** is not preserved when packing — the output PCK
  reports version 0.0.0. Use the command-line tool if you need to control this.
