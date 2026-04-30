# Godot PCK Web — Online PCK Inspector, Extractor & Repacker

**Inspect, extract, and repack Godot `.pck` files in your browser — no download or installation required.**

Works on Windows, macOS, and Linux. All processing happens locally; no files are ever uploaded anywhere.

👉 **[Open the tool](https://dtsykunov.github.io/GodotPckTool/)**

---

## What it does

- **List** — browse the file tree inside a `.pck`: paths, folder structure, and file sizes
- **Extract** — download all files from a `.pck` as a `.zip` you can open and edit locally
- **Pack** — turn a `.zip` back into a `.pck`, with your changes applied

This covers the most common need: inspect a Godot package, extract its contents, modify them, and repack — without installing anything.

---

## Typical workflow

```
Extract  →  edit files in your file manager  →  Pack
```

1. Use **Extract** to download all files from a `.pck` as a `.zip`.
2. Open the `.zip` and edit or replace files using your file manager.
3. When re-zipping, select the **files and folders inside** the extracted directory —
   do **not** compress the directory itself. See [Folder wrapping](#folder-wrapping) below.
4. Use **Pack** to turn the modified `.zip` back into a `.pck`.

---

## Choosing the Godot version when packing

> **The version selection affects the binary PCK format and is critical for compatibility.**
> A PCK packed with the wrong format will be rejected by the engine at runtime.
> Use **List** or **Extract** on your original `.pck` to see its version before packing.

The tool supports all three current PCK formats:

| Selection | PCK format |
|---|---|
| Godot 3.x | v1 |
| Godot 4.0 – 4.4 | v2 |
| Godot 4.5+ | v3 |

If a future Godot release introduces a new PCK format, it will not appear in this list and is not yet supported.

---

## Folder wrapping

A common mistake when re-zipping extracted files is compressing the containing
folder rather than its contents.

**Wrong** — compressing the folder itself:
```
my_game/           ← the extracted folder
  sprites/hero.png
  levels/world1.tres
```
This produces a ZIP where every entry starts with `my_game/`, so Pack would create
a PCK with paths `res://my_game/sprites/hero.png` — which the engine won't find.

**Correct** — compressing the contents:
```
sprites/hero.png
levels/world1.tres
```

**Auto-detection**: Pack automatically detects the common folder-wrapper pattern
(all entries share one top-level folder that itself contains subdirectories) and
strips it, notifying you in the status message. However, this heuristic does not
cover all cases, so prefer zipping correctly in the first place.

---

## Powered by GodotPckTool

Godot PCK Web is a browser-based interface built on top of
[GodotPckTool](https://github.com/hhyyrylainen/GodotPckTool) by
[@hhyyrylainen](https://github.com/hhyyrylainen) — the original command-line tool
for working with Godot `.pck` archives.

The C++ core is compiled to WebAssembly via Emscripten and runs entirely client-side.
All credit for the underlying PCK parsing and packing logic goes to the original
GodotPckTool contributors.

**For advanced use cases** — batch operations, scripting, regex filtering, JSON command files —
use the [GodotPckTool CLI](https://github.com/hhyyrylainen/GodotPckTool) directly.

---

## Limitations

- **Encrypted PCKs** are not supported — extraction and listing will fail.
- **No progress bar** for large archives — the page is unresponsive during processing; this is normal.
- **Godot version metadata** is not preserved exactly when packing — the output PCK records the
  selected version family (e.g. 4.0.0), not the exact original patch version. This does not affect compatibility.
- For anything beyond inspect / extract / repack, use the [GodotPckTool CLI](https://github.com/hhyyrylainen/GodotPckTool).

---

## Building

Requires [Emscripten](https://emscripten.org/) and CMake. The easiest way is via the Nix devShell:

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
python3 -m http.server 8080 --directory web/
```

Then open `http://localhost:8080` in your browser.

---

## Browser compatibility

Requires WebAssembly and ES2017 support:

- Chrome / Edge 57+
- Firefox 52+
- Safari 11+
