# Godot PCK Tool — Browser Edition

A fully client-side web interface for working with Godot `.pck` archive files.
No server, no installation, no data leaves your machine.

## Modes

### Extract → .zip
Drop a `.pck` file to extract all its contents and download them as a `.zip` archive.
The original directory structure inside the PCK is preserved.

### List contents
Drop a `.pck` file to inspect what is inside it. A table showing each file path and
its size appears in the page — nothing is downloaded.

### Repack (filter)
Drop a `.pck` file to repack it. Optionally filter which files are included using
ECMAScript regex patterns:

- **Include regex** — only files whose path matches this regex are kept.
  Leave empty to keep all files.
- **Exclude regex** — files whose path matches this regex are removed.
  Leave empty to remove nothing.

Examples:
- Include `\.png$` — keep only PNG textures
- Exclude `\.import$` — strip Godot import metadata files

The result downloads as `<original-name>-repacked.pck`.

### Add file
Add a single file into an existing `.pck` archive:

1. Drop the `.pck` file you want to modify into the main drop zone.
2. Drop (or select) the file you want to add using the second picker.
3. Edit the **Destination path** field — it pre-fills with `res://<filename>`.
   Change it to match where the file should live inside the archive
   (e.g. `res://sprites/hero.png`).
4. Click **Add to PCK**.

The result downloads as `<original-name>-modified.pck`.

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

# Any other static server works too
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
- **Add mode** inserts one file per operation. Run again to add more files.
- **No progress bar** for large archives — the page is unresponsive during
  extraction; this is normal.
- **Repack to a different Godot version** is not supported via the browser UI.
