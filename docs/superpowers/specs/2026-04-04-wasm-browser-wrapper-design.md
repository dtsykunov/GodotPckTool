# WASM Browser Wrapper — Design Spec

**Date:** 2026-04-04
**Status:** Approved

## Goal

Expose GodotPckTool's `.pck` extraction capability in the browser. The user drops a `.pck` file onto a static HTML page; the tool extracts it client-side using the existing C++ code compiled to WASM, and returns a `.zip` download. No server required.

**Constraint:** Minimal changes to existing `src/` files. All new code lives in `web/`. Upstream merge conflicts must be negligible.

---

## Repository Layout

New files only — nothing in `src/`, `CMakeLists.txt`, or existing `Makefile` targets is modified:

```
web/
├── CMakeLists.txt      # Emscripten build: links pck library, produces .js + .wasm
├── wasm_entry.cpp      # Single C entry point callable from JS
└── index.html          # Self-contained UI (inline CSS + JS, no framework)
```

Main `Makefile` gains two targets (appended at the bottom):

```makefile
compile-wasm:
    emcmake cmake -B build-wasm web/ && cmake --build build-wasm

install-wasm: compile-wasm
    cp build-wasm/godotpcktool.js build-wasm/godotpcktool.wasm web/
```

The `web/` directory is self-contained after `make install-wasm`: serve `index.html`, `godotpcktool.js`, and `godotpcktool.wasm` from the same directory.

---

## `web/CMakeLists.txt`

Pulls the existing `pck` library from `src/` via `add_subdirectory` — same source, compiled for WASM, no duplication:

```cmake
cmake_minimum_required(VERSION 3.10)
project(GodotPckToolWasm)

add_subdirectory(../src pck_build)
add_subdirectory(../third_party third_party_build)

add_executable(godotpcktool wasm_entry.cpp)
target_link_libraries(godotpcktool pck)

set_target_properties(godotpcktool PROPERTIES SUFFIX ".js")

target_link_options(godotpcktool PRIVATE
    -sEXPORTED_FUNCTIONS=['_extractPck','_getExtractedFiles','_getLastError','_malloc','_free']
    -sEXPORTED_RUNTIME_METHODS=['FS','HEAPU8']
    -sALLOW_MEMORY_GROWTH=1
    -sMODULARIZE=1
    -sEXPORT_NAME=GodotPckTool
)
```

**Note on `add_subdirectory(../src ...)`:** `src/CMakeLists.txt` defines both the `pck` library and the `godotpcktool` executable, and the executable uses platform flags (`-fuse-ld=gold`, `-static-libgcc`) that are irrelevant under Emscripten. `web/CMakeLists.txt` must also replicate the `configure_file` call that generates `Include.h` (normally done by the root `CMakeLists.txt`), and should set `CMAKE_FIND_LIBRARY_SUFFIXES` and `BUILD_SHARED_LIBS` consistently. The `godotpcktool` executable target will be compiled as WASM too but is unused; this is acceptable.

Key flag rationale:
- `MODULARIZE=1` + `EXPORT_NAME` — factory function, safe for lazy loading, no global pollution
- `ALLOW_MEMORY_GROWTH` — `.pck` files can be large; fixed heap would OOM
- `FS` + `HEAPU8` — JS needs these to write input bytes and read extracted files from MEMFS
- `_malloc`/`_free` — JS allocates a buffer in the WASM heap to pass file bytes in

---

## `web/wasm_entry.cpp`

Three exported C functions; no changes to any existing `.cpp` file:

```cpp
extern "C" {
    // Write data into MEMFS, load and extract the PCK, populate manifest.
    // Returns 0 on success, non-zero on error (call getLastError() for message).
    int extractPck(const uint8_t* data, size_t len);

    // After a successful extractPck(), returns newline-separated list of
    // paths written under /out/ in MEMFS.
    const char* getExtractedFiles();

    // After a failed extractPck(), returns a human-readable error string.
    const char* getLastError();
}
```

Internal flow of `extractPck()`:
1. `fprintf(stderr, ...)` trace: bytes received
2. Write `data` to `/input.pck` in MEMFS
3. Construct `PckFile`, call `Load()` — log and set error on failure
4. Call `Extract("/out/")` — log and set error on failure
5. Walk `/out/` recursively, build newline-separated manifest of paths **relative to `/out/`** (e.g. `res://icon.png` not `/out/res://icon.png`) so the zip preserves the original directory structure without the MEMFS prefix
6. `fprintf(stderr, ...)` trace: file count extracted
7. Return 0

All `stderr` output routes to `console.error` automatically via Emscripten.

---

## `index.html` — UI & JS Glue

Single self-contained file. Inline CSS and JS. No framework, no build step.

### UI States

| State | What the user sees |
|---|---|
| Idle | Drag-and-drop zone + file picker button |
| Loading WASM | Spinner ("Loading extractor…") |
| Processing | "Extracting `<filename>`…" |
| Done | "Download `<name>.zip`" button + file count |
| Error | Red message from `getLastError()` |

### JS Flow

```
User drops/picks .pck
  → [lazy] fetch godotpcktool.js + .wasm → GodotPckTool() factory
  → Module._malloc(len)
  → Module.HEAPU8.set(fileBytes, ptr)
  → Module._extractPck(ptr, len)
  → Module._free(ptr)
  → read Module._getExtractedFiles(), split on '\n'
  → for each path: Module.FS.readFile(path) → JSZip.file(path, bytes)
  → zip.generateAsync({type:'blob'}) → <a download> trigger
```

JSZip loaded from CDN (alongside WASM, on first file drop).

### Logging

All steps logged to `console.log`/`console.error`:
- WASM fetch start/end, bytes received
- `extractPck` call with file name and size
- File count from manifest
- Zip generation size
- Any error from `getLastError()`

---

## Error Handling

| Failure point | C++ side | JS side |
|---|---|---|
| Not a valid PCK file | `PckFile::Load()` fails → `getLastError()` set | Red error message shown |
| Unsupported PCK version | Same | Same |
| WASM fetch fails | — | `fetch` rejection → "Failed to load extractor" |
| OOM (very large file) | `ALLOW_MEMORY_GROWTH` mitigates | try/catch → error message |
| Zero files extracted | Manifest is empty | "No files found in this .pck" |

---

## Build Dependencies (Nix devShell)

`flake.nix` at repo root provides:
- `cmake`, `ninja`, `gcc`, `binutils` — native build
- `emscripten`, `nodejs_22` — WASM build
- `git` — submodule management

Enter with `nix develop`, then `make compile-wasm`.

---

## What Is Not In Scope

- Repack, add, list actions in the browser — extract only
- Server-side processing
- Encryption support (Godot encrypted PCKs)
- Progress reporting for large archives (single-shot extraction)
