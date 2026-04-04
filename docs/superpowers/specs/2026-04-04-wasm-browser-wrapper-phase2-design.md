# WASM Browser Wrapper — Phase 2 Design Spec

**Date:** 2026-04-04
**Status:** Approved

## Goal

Extend the existing browser-based PCK extractor (Phase 1) to expose the full CLI feature set: **list**, **extract** (unchanged), **repack** (with optional regex filters), and **add** (single file with user-specified destination path). All work remains in `web/`; changes to `src/` are zero.

---

## Architecture

Three new C functions join `wasm_entry.cpp`. `index.html` gains a mode selector, per-mode description text, and per-mode controls. `web/CMakeLists.txt` gets an updated `EXPORTED_FUNCTIONS` list. A `web/README.md` documents the full feature set. No other files change.

### Files changed

| File | Change |
|---|---|
| `web/wasm_entry.cpp` | Add `listPck`, `repackPck`, `addToPck` |
| `web/index.html` | Mode selector, description panel, per-mode controls, file-list table |
| `web/CMakeLists.txt` | Add 3 new symbols to `EXPORTED_FUNCTIONS` |
| `web/README.md` | New — documents all modes, build, and serve instructions |

---

## New C Functions (`wasm_entry.cpp`)

All three follow the same conventions as the existing `extractPck`: clear state at entry, log to `stderr`, set `s_lastError` on failure.

### `listPck`

```cpp
const char* listPck(const uint8_t* data, std::size_t len);
```

Flow:
1. Clear `s_manifest` and `s_lastError`.
2. Cleanup MEMFS (`/out/`, `/input.pck`).
3. Write `data` to `/input.pck`.
4. Construct `PckFile("/input.pck")`, call `Load()` — set error and return `nullptr` on failure.
5. Call `Extract("/out/", false)` — set error and return `nullptr` on failure.
6. Walk `/out/` recursively; for each regular file collect relative path and `std::filesystem::file_size`. Build `s_manifest` as newline-separated `path\tsize` pairs (size in bytes).
7. Log file count to `stderr`. Return `s_manifest.c_str()`.

Returns `nullptr` (not empty string) on error so JS can distinguish error from empty archive.

### `repackPck`

```cpp
int repackPck(const uint8_t* data, std::size_t len,
              const char* include_regex, const char* exclude_regex);
```

Flow:
1. Clear `s_lastError`. Remove `/input.pck` and `/output.pck` from MEMFS.
2. Write `data` to `/input.pck`.
3. Construct `PckFile("/input.pck")`.
4. If `include_regex` or `exclude_regex` is non-null and non-empty, build a `FileFilter`, call `SetIncludeRegexes`/`SetExcludeRegexes`, and call `pck.SetIncludeFilter(...)`. Wrap regex construction in try/catch — invalid regex sets `s_lastError` and returns 1.
5. Call `Load()` — set error and return 2 on failure.
6. Call `pck.ChangePath("/output.pck")`.
7. Call `pck.Save()` — set error and return 3 on failure.
8. Log to `stderr`. Return 0.

JS reads result with `Module.FS.readFile('/output.pck')` and triggers a `.pck` download.

### `addToPck`

```cpp
int addToPck(const uint8_t* pck_data, std::size_t pck_len,
             const uint8_t* file_data, std::size_t file_len,
             const char* pck_path);
```

Flow:
1. Clear `s_lastError`. Remove `/input.pck`, `/add_input`, `/output.pck` from MEMFS.
2. Write `pck_data` to `/input.pck`.
3. Write `file_data` to `/add_input`.
4. Construct `PckFile("/input.pck")`, call `Load()` — set error and return 1 on failure.
5. Call `pck.ChangePath("/output.pck")`.
6. Call `pck.AddSingleFile("/add_input", pck_path, false)`.
7. Call `pck.Save()` — set error and return 2 on failure.
8. Log to `stderr`. Return 0.

JS reads result with `Module.FS.readFile('/output.pck')` and triggers a `.pck` download.

### `EXPORTED_FUNCTIONS` update (`web/CMakeLists.txt`)

```cmake
"SHELL:-sEXPORTED_FUNCTIONS=['_extractPck','_getExtractedFiles','_getLastError','_listPck','_repackPck','_addToPck','_malloc','_free']"
"SHELL:-sEXPORTED_RUNTIME_METHODS=['FS','HEAPU8','UTF8ToString','stringToUTF8','lengthBytesUTF8']"
```

`stringToUTF8` and `lengthBytesUTF8` are needed so JS can allocate and write regex and path strings into the WASM heap.

---

## `index.html` — UI & JS

### Layout

```
[h1] Godot PCK Tool
[subtitle]

[Mode: Extract ▼]             ← <select id="mode-select">, always visible

[description panel]           ← <p id="mode-desc">, updates on mode change

[  drop .pck here  ]          ← <div id="drop-zone">, always visible
<input type="file" id="file-input" accept=".pck">

[#repack-controls]            ← shown in repack mode only
  Include regex: [_________]
  Exclude regex: [_________]

[#add-controls]               ← shown in add mode only
  [  drop / select file to add  ]
  <input type="file" id="add-file-input">
  Destination: [res://______]

[#status]
[#download-btn]               ← extract / repack / add result
[#list-output]                ← list result table (path, size columns)
```

### Per-mode description text (hard-coded, set via `textContent`)

| Mode | Description |
|---|---|
| **Extract** | "Drop a `.pck` file to extract all its contents and download them as a `.zip` archive." |
| **List** | "Drop a `.pck` file to inspect its contents. A table of file paths and sizes will appear below — nothing is downloaded." |
| **Repack** | "Drop a `.pck` file to repack it, optionally filtering files by regex. Leave both fields empty to repack as-is. The result downloads as a new `.pck` file." |
| **Add** | "Drop a `.pck` file, then select a file to add and set its destination path inside the archive (e.g. `res://sprites/hero.png`). The modified `.pck` downloads." |

### Mode change behaviour

Switching mode:
- Updates `#mode-desc` text.
- Shows/hides `#repack-controls` and `#add-controls`.
- Clears `#status`, hides `#download-btn`, clears `#list-output`.
- Resets `isProcessing` guard if not mid-operation.
- Resets stored file references for add mode (`selectedPckFile`, `selectedAddFile`).

### JS state

```js
let wasmModule     = null;   // cached Emscripten module
let jszipLib       = null;   // cached JSZip (extract mode only)
let activeBlobUrl  = null;   // tracked for revocation
let isProcessing   = false;  // guard against concurrent operations
let selectedPckFile = null;  // add mode: the .pck file
let selectedAddFile = null;  // add mode: the file to insert
```

### Per-mode JS flows

**Extract** — unchanged from Phase 1.

**List:**
```
drop/pick .pck
  → getWasmModule()
  → write bytes to WASM heap, call Module._listPck(ptr, len)
  → Module._free(ptr)
  → on null result: show error from _getLastError()
  → on success: UTF8ToString result, split on '\n', split each on '\t'
  → populate #file-table rows via createElement/appendChild (no innerHTML)
  → show #list-output, set status "N files in <name>"
```

**Repack:**
```
drop/pick .pck
  → getWasmModule()
  → read include/exclude regex input values (trim whitespace)
  → write bytes to WASM heap, call Module._repackPck(ptr, len, includePtr, excludePtr)
  → Module._free(ptrs)
  → on error: show getLastError()
  → on success: Module.FS.readFile('/output.pck') → Blob → showDownload(blob, name+'.pck')
```

Regex strings are written to WASM heap with `Module.stringToUTF8` / `Module._malloc`. Empty string = no filter (C++ side checks for empty).

**Add:**
```
User drops/picks .pck      → store in selectedPckFile, update status
User drops/picks add-file  → store in selectedAddFile, update status
                           → auto-fill #pck-path with "res://" + filename if empty

When both files are set, an "Add to PCK" button appears. User clicks it to run:
  → getWasmModule()
  → allocate heap buffers for pck bytes, file bytes, pck_path string
  → Module._addToPck(pckPtr, pckLen, filePtr, fileLen, pathPtr)
  → free all pointers
  → on error: show getLastError()
  → on success: FS.readFile('/output.pck') → Blob → showDownload
```

The button appears (and re-runs) whenever both files are present, allowing the user to adjust the path and re-run without re-selecting files.

### XSS safety

All user-controlled content (file names, sizes, paths returned from WASM) goes through `textContent` or `document.createTextNode`. Table rows built with `createElement`/`appendChild`. No `innerHTML` with user data anywhere.

---

## Error Handling

| Failure | C++ | JS |
|---|---|---|
| Invalid / corrupt PCK | `Load()` returns false → `s_lastError` set | Red status message |
| Invalid regex in repack | try/catch in `repackPck` → `s_lastError` | Red status message |
| Save failure | `Save()` returns false → `s_lastError` | Red status message |
| WASM fetch fails | — | `loadScript` rejection → "Failed to load extractor" |
| No files in PCK | Empty manifest | "No files found in this .pck" |
| Missing add-file or path | — | JS validates before calling WASM, prompts user |

---

## `web/README.md`

Documents:

1. **Overview** — browser-based PCK tool, fully client-side, no server, no install required.
2. **Modes** — Extract, List, Repack, Add — each with a short description.
3. **Building** — `nix develop` for the devShell, then `make install-wasm`. Raw Emscripten alternative: `emcmake cmake -B build-wasm web/ && cmake --build build-wasm --parallel && cp build-wasm/godotpcktool.{js,wasm} web/`.
4. **Serving** — any static file server from the `web/` directory, e.g. `python3 -m http.server 8080` or `npx serve web/`. Cannot be opened as a local `file://` URL due to WASM fetch restrictions.
5. **Browser compatibility** — modern browsers with WebAssembly and ES2017 support (Chrome 57+, Firefox 52+, Safari 11+).
6. **Limitations** — encrypted PCKs not supported; Add mode inserts one file at a time; no progress reporting for large archives.

---

## What Is Not In Scope

- Batch add (multiple files in one operation)
- Progress reporting for large archives
- Encryption support
- Repack to a different Godot version
