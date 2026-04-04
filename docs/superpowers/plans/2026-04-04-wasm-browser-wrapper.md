# WASM Browser Wrapper Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a browser-based `.pck` extractor — user drops a file, gets a `.zip` — using the existing C++ `pck` library compiled to WASM via Emscripten, served as static files with no server required.

**Architecture:** A thin `web/wasm_entry.cpp` bridges JS to the existing `PckFile` C++ class. `web/CMakeLists.txt` builds it as a standalone Emscripten project referencing `../src` and `../third_party`. A single-file `index.html` handles drag-and-drop, lazy-loads the WASM module + JSZip, and triggers a `.zip` download. One minimal guard is added to `src/CMakeLists.txt` to prevent the CLI executable's platform-specific linker flags from breaking the WASM build.

**Tech Stack:** Emscripten (C++17 → WASM), CMake 3.10+, JSZip 3.x (CDN), vanilla JS/HTML/CSS, Nix devShell (`nix develop`).

---

## File Map

| File | Action | Responsibility |
|---|---|---|
| `src/CMakeLists.txt` | Modify (3 lines) | Wrap CLI executable in `if(BUILD_GODOTPCKTOOL_EXECUTABLE)` guard |
| `web/CMakeLists.txt` | Create | Emscripten project: replicates root project setup, links `pck` library, sets WASM flags |
| `web/wasm_entry.cpp` | Create | Three C-linkage functions: `extractPck`, `getExtractedFiles`, `getLastError` |
| `web/index.html` | Create | Self-contained UI: drag-and-drop, lazy WASM load, JSZip download |
| `Makefile` | Modify (append) | `compile-wasm` and `install-wasm` targets |

---

## Task 1: Guard CLI executable in `src/CMakeLists.txt`

**Why:** `src/CMakeLists.txt` always creates the `godotpcktool` CLI executable, which sets `-fuse-ld=gold` in Release builds. Under Emscripten the linker is `emcc` and this flag is invalid. Adding an `option()` guard (defaulting `ON`) leaves all existing builds unaffected.

**Files:**
- Modify: `src/CMakeLists.txt`

- [ ] **Step 1: Add the option and wrap the executable block**

  Open `src/CMakeLists.txt`. Add one line before `add_executable(godotpcktool main.cpp)` and wrap everything from `add_executable` through `install(TARGETS godotpcktool)` in an `if/endif` block.

  Replace this block (lines 19–47):
  ```cmake
  add_executable(godotpcktool main.cpp)

  target_link_libraries(godotpcktool PRIVATE pck)

  # Static standard lib
  target_link_libraries(godotpcktool PRIVATE -static-libgcc -static-libstdc++)

  # Fully static executable
  if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    target_link_libraries(godotpcktool PRIVATE -static)
  endif()

  if(NOT WIN32)
    set_target_properties(godotpcktool PROPERTIES LINK_FLAGS_RELEASE "-s -fuse-ld=gold")
  else()
    set_target_properties(godotpcktool PROPERTIES LINK_FLAGS_RELEASE "-s")
  endif()

  set_target_properties(godotpcktool PROPERTIES
    CXX_STANDARD 17
    CXX_EXTENSIONS OFF
    )

  check_ipo_supported(RESULT result)
  if(result)
    set_target_properties(godotpcktool PROPERTIES INTERPROCEDURAL_OPTIMIZATION TRUE)
  endif()

  install(TARGETS godotpcktool)
  ```

  With this:
  ```cmake
  option(BUILD_GODOTPCKTOOL_EXECUTABLE "Build the godotpcktool CLI executable" ON)

  if(BUILD_GODOTPCKTOOL_EXECUTABLE)
    add_executable(godotpcktool main.cpp)

    target_link_libraries(godotpcktool PRIVATE pck)

    # Static standard lib
    target_link_libraries(godotpcktool PRIVATE -static-libgcc -static-libstdc++)

    # Fully static executable
    if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
      target_link_libraries(godotpcktool PRIVATE -static)
    endif()

    if(NOT WIN32)
      set_target_properties(godotpcktool PROPERTIES LINK_FLAGS_RELEASE "-s -fuse-ld=gold")
    else()
      set_target_properties(godotpcktool PROPERTIES LINK_FLAGS_RELEASE "-s")
    endif()

    set_target_properties(godotpcktool PROPERTIES
      CXX_STANDARD 17
      CXX_EXTENSIONS OFF
      )

    check_ipo_supported(RESULT result)
    if(result)
      set_target_properties(godotpcktool PROPERTIES INTERPROCEDURAL_OPTIMIZATION TRUE)
    endif()

    install(TARGETS godotpcktool)
  endif()
  ```

- [ ] **Step 2: Verify the native build is unaffected**

  Run:
  ```bash
  cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build --target godotpcktool
  ```
  Expected: builds successfully, `build/src/godotpcktool` exists.

- [ ] **Step 3: Commit**

  ```bash
  git add src/CMakeLists.txt
  git commit -m "src: add BUILD_GODOTPCKTOOL_EXECUTABLE option to guard CLI target

  Allows the WASM build (web/CMakeLists.txt) to include src/ without
  inheriting platform-specific linker flags (-fuse-ld=gold, -static-libgcc)
  that are invalid under Emscripten. Default is ON so native builds are
  unaffected."
  ```

---

## Task 2: Create `web/CMakeLists.txt`

**Files:**
- Create: `web/CMakeLists.txt`

- [ ] **Step 1: Create the file**

  ```cmake
  # Emscripten build for browser-based .pck extractor
  # Build with: emcmake cmake -B build-wasm web/ && cmake --build build-wasm
  cmake_minimum_required(VERSION 3.10)
  project(GodotPckToolWasm)

  # Mirror version from root CMakeLists.txt — update when root version changes
  set(GODOT_PCK_TOOL_VERSION_MAJOR 2)
  set(GODOT_PCK_TOOL_VERSION_MINOR 2)
  set(GODOT_PCK_TOOL_VERSION_STR "${GODOT_PCK_TOOL_VERSION_MAJOR}.${GODOT_PCK_TOOL_VERSION_MINOR}")

  # Generate Include.h (mirrors the configure_file call in root CMakeLists.txt)
  configure_file("../src/Include.h.in" "${PROJECT_BINARY_DIR}/Include.h")

  # Global includes needed by src/ (mirrors root CMakeLists.txt include_directories)
  include_directories(${PROJECT_BINARY_DIR})
  include_directories(../src)
  include_directories(../third_party/json/single_include)

  set(BUILD_SHARED_LIBS OFF)

  if(CMAKE_BUILD_TYPE STREQUAL "")
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
  endif()

  # Third-party libs (md5, etc.)
  add_subdirectory(../third_party third_party_build)

  # pck library only — skip CLI executable (it has platform flags invalid under Emscripten)
  set(BUILD_GODOTPCKTOOL_EXECUTABLE OFF CACHE BOOL "" FORCE)
  add_subdirectory(../src src_build)

  # WASM entry point
  add_executable(godotpcktool wasm_entry.cpp)
  target_link_libraries(godotpcktool PRIVATE pck)

  set_target_properties(godotpcktool PROPERTIES
      SUFFIX ".js"
      CXX_STANDARD 17
      CXX_EXTENSIONS OFF
  )

  # SHELL: prefix prevents CMake from deduplicating/splitting bracketed argument lists
  target_link_options(godotpcktool PRIVATE
      "SHELL:-sEXPORTED_FUNCTIONS=['_extractPck','_getExtractedFiles','_getLastError','_malloc','_free']"
      "SHELL:-sEXPORTED_RUNTIME_METHODS=['FS','HEAPU8','UTF8ToString']"
      -sALLOW_MEMORY_GROWTH=1
      -sMODULARIZE=1
      -sEXPORT_NAME=GodotPckTool
  )
  ```

- [ ] **Step 2: Verify CMake configure step succeeds (no build yet)**

  Run:
  ```bash
  emcmake cmake -B build-wasm web/
  ```
  Expected output includes:
  ```
  -- Configuring done
  -- Build files have been written to: .../build-wasm
  ```
  No errors. Verify `Include.h` was generated:
  ```bash
  cat build-wasm/Include.h
  ```
  Expected: contains `#define GODOT_PCK_TOOL_VERSION 2.2`

- [ ] **Step 3: Commit**

  ```bash
  git add web/CMakeLists.txt
  git commit -m "web: add Emscripten CMake build configuration"
  ```

---

## Task 3: Create `web/wasm_entry.cpp`

**Context on `PckFile::Extract`:** It strips `res://` from paths before writing to disk, so a PCK entry `res://icon.png` becomes `/out/icon.png` in MEMFS. The manifest returned by `getExtractedFiles()` contains `icon.png` (relative to `/out/`). The JS side reads `Module.FS.readFile('/out/' + relPath)`.

**Files:**
- Create: `web/wasm_entry.cpp`

- [ ] **Step 1: Create the file**

  ```cpp
  #include "pck/PckFile.h"

  #include <cstdint>
  #include <cstdio>
  #include <filesystem>
  #include <fstream>
  #include <string>

  static std::string s_manifest;
  static std::string s_lastError;

  // Walk dir recursively, appending paths relative to rootLen to out.
  static void walkDir(const std::filesystem::path& dir, std::size_t rootLen, std::string& out)
  {
      for(const auto& entry : std::filesystem::recursive_directory_iterator(dir)) {
          if(!entry.is_regular_file())
              continue;
          std::string full = entry.path().string();
          // Strip leading root prefix to get relative path
          std::string rel = full.substr(rootLen);
          // Remove leading slash
          if(!rel.empty() && rel.front() == '/')
              rel.erase(rel.begin());
          if(!out.empty())
              out += '\n';
          out += rel;
      }
  }

  extern "C" {

  int extractPck(const uint8_t* data, std::size_t len)
  {
      s_manifest.clear();
      s_lastError.clear();

      fprintf(stderr, "[godotpcktool] extractPck: %zu bytes received\n", len);

      // Clean up any previous extraction
      std::error_code ec;
      std::filesystem::remove_all("/out/", ec);
      std::filesystem::remove("/input.pck", ec);

      // Write bytes to MEMFS
      {
          std::ofstream f("/input.pck", std::ios::binary);
          if(!f) {
              s_lastError = "Failed to write input file to virtual filesystem";
              fprintf(stderr, "[godotpcktool] ERROR: %s\n", s_lastError.c_str());
              return 1;
          }
          f.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(len));
      }
      fprintf(stderr, "[godotpcktool] Written to /input.pck\n");

      // Load PCK
      pcktool::PckFile pck("/input.pck");
      if(!pck.Load()) {
          s_lastError =
              "Failed to load PCK. The file may be invalid, corrupted, or an unsupported version.";
          fprintf(stderr, "[godotpcktool] ERROR: Load() failed\n");
          return 2;
      }
      fprintf(stderr, "[godotpcktool] Loaded: Godot %s, format version %u\n",
          pck.GetGodotVersion().c_str(), pck.GetFormatVersion());

      // Extract all files to /out/
      if(!pck.Extract("/out/", false)) {
          s_lastError = "Extraction failed. The PCK may be encrypted or contain unsupported data.";
          fprintf(stderr, "[godotpcktool] ERROR: Extract() failed\n");
          return 3;
      }
      fprintf(stderr, "[godotpcktool] Extract() complete\n");

      // Build manifest of relative paths
      std::string root("/out/");
      walkDir(root, root.size(), s_manifest);

      const int count = s_manifest.empty() ?
          0 :
          static_cast<int>(std::count(s_manifest.begin(), s_manifest.end(), '\n')) + 1;
      fprintf(stderr, "[godotpcktool] %d files extracted\n", count);

      if(s_manifest.empty()) {
          s_lastError = "No files found in this PCK.";
          return 4;
      }

      return 0;
  }

  const char* getExtractedFiles()
  {
      return s_manifest.c_str();
  }

  const char* getLastError()
  {
      return s_lastError.c_str();
  }

  } // extern "C"
  ```

- [ ] **Step 2: Build the WASM module**

  Run:
  ```bash
  emcmake cmake -B build-wasm web/ && cmake --build build-wasm
  ```
  Expected: build succeeds, producing:
  ```
  build-wasm/godotpcktool.js
  build-wasm/godotpcktool.wasm
  ```
  Verify:
  ```bash
  ls -lh build-wasm/godotpcktool.js build-wasm/godotpcktool.wasm
  ```
  Expected: both files exist, `.wasm` is typically 500 KB–2 MB, `.js` is 50–200 KB.

- [ ] **Step 3: Commit**

  ```bash
  git add web/wasm_entry.cpp
  git commit -m "web: add WASM entry point (extractPck, getExtractedFiles, getLastError)"
  ```

---

## Task 4: Add `compile-wasm` and `install-wasm` to `Makefile`

**Files:**
- Modify: `Makefile` (two new targets + extend `clean`)

- [ ] **Step 1: Append WASM targets and extend clean**

  Add the following at the very end of `Makefile`:

  ```makefile
  # WASM / browser build (requires Emscripten: nix develop or emsdk)
  compile-wasm:
  	emcmake cmake -B build-wasm web/ && cmake --build build-wasm

  install-wasm: compile-wasm
  	cp build-wasm/godotpcktool.js build-wasm/godotpcktool.wasm web/

  .PHONY: compile-wasm install-wasm
  ```

  Also update the existing `clean` target to include `build-wasm`:
  ```makefile
  # Before (existing):
  clean: remove-podman-image
  	rm -rf build build-cross build-podman install

  # After:
  clean: remove-podman-image
  	rm -rf build build-cross build-podman build-wasm install
  ```

  **Important:** Makefile rules must use **tab** characters for indentation, not spaces.

- [ ] **Step 2: Verify `make compile-wasm` works**

  Run:
  ```bash
  make compile-wasm
  ```
  Expected: CMake configures and builds without errors. `build-wasm/godotpcktool.js` and `build-wasm/godotpcktool.wasm` exist.

- [ ] **Step 3: Verify `make install-wasm` copies files**

  Run:
  ```bash
  make install-wasm && ls web/
  ```
  Expected output includes `godotpcktool.js` and `godotpcktool.wasm`.

- [ ] **Step 4: Commit**

  ```bash
  git add Makefile
  git commit -m "Makefile: add compile-wasm and install-wasm targets"
  ```

---

## Task 5: Create `web/index.html`

**Files:**
- Create: `web/index.html`

**Context:** `GodotPckTool()` (from `godotpcktool.js`) is a factory that returns a Promise resolving to the Emscripten module. The module exposes `._malloc`, `._free`, `._extractPck`, `._getExtractedFiles`, `._getLastError`, `.HEAPU8`, `.FS`, and `.UTF8ToString`. JSZip is loaded from CDN alongside the WASM module.

**XSS note:** `file.name` comes from the user's filesystem and must never be injected into `innerHTML`. All user-controlled strings are set via `textContent` or safe DOM methods.

- [ ] **Step 1: Create the file**

  ```html
  <!DOCTYPE html>
  <html lang="en">
  <head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Godot PCK Extractor</title>
    <style>
      * { box-sizing: border-box; margin: 0; padding: 0; }
      body {
        font-family: system-ui, sans-serif;
        background: #1a1a2e;
        color: #e0e0e0;
        min-height: 100vh;
        display: flex;
        flex-direction: column;
        align-items: center;
        justify-content: center;
        padding: 2rem;
      }
      h1 { font-size: 1.6rem; margin-bottom: 0.4rem; color: #fff; }
      p.subtitle { color: #aaa; margin-bottom: 2rem; font-size: 0.95rem; }
      #drop-zone {
        border: 2px dashed #4a90d9;
        border-radius: 12px;
        padding: 3rem 4rem;
        text-align: center;
        cursor: pointer;
        transition: background 0.2s, border-color 0.2s;
        width: 100%;
        max-width: 480px;
      }
      #drop-zone.drag-over { background: #1e3a5f; border-color: #7ab8f5; }
      #drop-zone p { color: #aaa; margin-top: 0.5rem; font-size: 0.9rem; }
      #file-input { display: none; }
      #status {
        margin-top: 1.5rem;
        min-height: 2rem;
        text-align: center;
        font-size: 0.95rem;
        width: 100%;
        max-width: 480px;
      }
      #status.error { color: #ff6b6b; }
      #status.success { color: #6bffb8; }
      .spinner {
        display: inline-block;
        width: 1em; height: 1em;
        border: 2px solid #4a90d9;
        border-top-color: transparent;
        border-radius: 50%;
        animation: spin 0.7s linear infinite;
        vertical-align: middle;
        margin-right: 0.4em;
      }
      @keyframes spin { to { transform: rotate(360deg); } }
      #download-btn {
        display: none;
        margin-top: 1rem;
        padding: 0.7rem 2rem;
        background: #4a90d9;
        color: #fff;
        border: none;
        border-radius: 8px;
        font-size: 1rem;
        cursor: pointer;
        text-decoration: none;
      }
      #download-btn:hover { background: #357abd; }
    </style>
  </head>
  <body>
    <h1>Godot PCK Extractor</h1>
    <p class="subtitle">Drop a <code>.pck</code> file to extract its contents as a <code>.zip</code></p>

    <div id="drop-zone">
      <svg width="48" height="48" fill="none" stroke="#4a90d9" stroke-width="2"
           viewBox="0 0 24 24" aria-hidden="true">
        <path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4"/>
        <polyline points="17 8 12 3 7 8"/>
        <line x1="12" y1="3" x2="12" y2="15"/>
      </svg>
      <p>Drag &amp; drop a .pck file here, or click to select</p>
      <input type="file" id="file-input" accept=".pck">
    </div>

    <div id="status"></div>
    <a id="download-btn">Download .zip</a>

    <script>
      // ── State ────────────────────────────────────────────────────────────────
      let wasmModule = null;  // cached Emscripten module instance
      let jszipLib   = null;  // cached JSZip class

      // ── UI helpers ───────────────────────────────────────────────────────────
      const dropZone    = document.getElementById('drop-zone');
      const fileInput   = document.getElementById('file-input');
      const statusEl    = document.getElementById('status');
      const downloadBtn = document.getElementById('download-btn');

      // All user-controlled content goes through textContent, never innerHTML.
      function setStatus(message, cls) {
        statusEl.textContent = message;
        statusEl.className   = cls || '';
      }

      // Spinner uses only trusted static markup — no user content.
      function setStatusSpinner(staticMessage) {
        statusEl.className = '';
        statusEl.textContent = '';
        const spinner = document.createElement('span');
        spinner.className = 'spinner';
        statusEl.appendChild(spinner);
        // staticMessage is a hard-coded string from our own code, safe to use as text
        statusEl.appendChild(document.createTextNode(' ' + staticMessage));
      }

      function showDownload(blob, filename) {
        const url = URL.createObjectURL(blob);
        downloadBtn.href = url;
        downloadBtn.download = filename;
        downloadBtn.style.display = 'inline-block';
      }

      function hideDownload() {
        downloadBtn.style.display = 'none';
        downloadBtn.href = '';
      }

      // ── Lazy loaders ─────────────────────────────────────────────────────────
      function loadScript(src) {
        return new Promise((resolve, reject) => {
          const s = document.createElement('script');
          s.src = src;
          s.onload = resolve;
          s.onerror = () => reject(new Error('Failed to load script: ' + src));
          document.head.appendChild(s);
        });
      }

      async function getWasmModule() {
        if (wasmModule) return wasmModule;
        console.log('[godotpcktool] Loading WASM module...');
        await loadScript('godotpcktool.js');
        // GodotPckTool is now a global factory (MODULARIZE=1, EXPORT_NAME=GodotPckTool)
        wasmModule = await GodotPckTool();
        console.log('[godotpcktool] WASM module ready');
        return wasmModule;
      }

      async function getJSZip() {
        if (jszipLib) return jszipLib;
        console.log('[godotpcktool] Loading JSZip...');
        await loadScript(
          'https://cdnjs.cloudflare.com/ajax/libs/jszip/3.10.1/jszip.min.js'
        );
        jszipLib = window.JSZip;
        console.log('[godotpcktool] JSZip ready');
        return jszipLib;
      }

      // ── Core extraction logic ─────────────────────────────────────────────────
      async function extractAndZip(file) {
        hideDownload();
        setStatusSpinner('Loading extractor\u2026');

        let Module, JSZip;
        try {
          [Module, JSZip] = await Promise.all([getWasmModule(), getJSZip()]);
        } catch (e) {
          console.error('[godotpcktool] Load error:', e);
          setStatus('Failed to load extractor: ' + e.message, 'error');
          return;
        }

        setStatusSpinner('Extracting ' + file.name + '\u2026');

        let bytes;
        try {
          const buf = await file.arrayBuffer();
          bytes = new Uint8Array(buf);
        } catch (e) {
          setStatus('Failed to read file: ' + e.message, 'error');
          return;
        }

        console.log('[godotpcktool] Calling extractPck:', file.name, bytes.length, 'bytes');

        const ptr = Module._malloc(bytes.length);
        Module.HEAPU8.set(bytes, ptr);
        const result = Module._extractPck(ptr, bytes.length);
        Module._free(ptr);

        if (result !== 0) {
          const err = Module.UTF8ToString(Module._getLastError());
          console.error('[godotpcktool] extractPck error:', err);
          setStatus(err, 'error');
          return;
        }

        const manifestStr = Module.UTF8ToString(Module._getExtractedFiles());
        const paths = manifestStr.split('\n').filter(Boolean);
        console.log('[godotpcktool] Files to zip:', paths.length);

        const zip = new JSZip();
        for (const relPath of paths) {
          const data = Module.FS.readFile('/out/' + relPath);
          zip.file(relPath, data);
        }

        let blob;
        try {
          blob = await zip.generateAsync({ type: 'blob' });
        } catch (e) {
          setStatus('Failed to generate zip: ' + e.message, 'error');
          return;
        }

        const zipName = file.name.replace(/\.pck$/i, '') + '.zip';
        console.log('[godotpcktool] Zip ready:', zipName, blob.size, 'bytes');

        const fileWord = paths.length === 1 ? 'file' : 'files';
        setStatus('Extracted ' + paths.length + ' ' + fileWord + ' from ' + file.name, 'success');
        showDownload(blob, zipName);
      }

      // ── Drag-and-drop + click ─────────────────────────────────────────────────
      dropZone.addEventListener('click', () => fileInput.click());

      fileInput.addEventListener('change', () => {
        if (fileInput.files[0]) extractAndZip(fileInput.files[0]);
      });

      dropZone.addEventListener('dragover', e => {
        e.preventDefault();
        dropZone.classList.add('drag-over');
      });

      dropZone.addEventListener('dragleave', () => {
        dropZone.classList.remove('drag-over');
      });

      dropZone.addEventListener('drop', e => {
        e.preventDefault();
        dropZone.classList.remove('drag-over');
        const file = e.dataTransfer.files[0];
        if (file) extractAndZip(file);
      });
    </script>
  </body>
  </html>
  ```

- [ ] **Step 2: Commit**

  ```bash
  git add web/index.html
  git commit -m "web: add self-contained browser UI for PCK extraction"
  ```

---

## Task 6: End-to-end smoke test

**Prerequisites:** `make install-wasm` has been run so `web/` contains `index.html`, `godotpcktool.js`, and `godotpcktool.wasm`.

**You need:** A real `.pck` file. Any Godot 3.x or 4.x game export works. The Godot editor can export a minimal project to produce one.

- [ ] **Step 1: Serve `web/` over HTTP**

  WASM modules cannot be loaded via `file://` due to CORS restrictions; HTTP is required.

  Run:
  ```bash
  cd web && python3 -m http.server 8080
  ```
  Open `http://localhost:8080` in a browser.

- [ ] **Step 2: Test happy path**

  1. Drop a `.pck` file onto the drop zone (or click to select one)
  2. Observe spinner → "Extracting `<filename>`…" → success message with file count and download button
  3. Click "Download .zip", unzip, verify contents match the original PCK

  Open browser devtools (F12) → Console. Expected log output:
  ```
  [godotpcktool] Loading WASM module...
  [godotpcktool] WASM module ready
  [godotpcktool] Loading JSZip...
  [godotpcktool] JSZip ready
  [godotpcktool] Calling extractPck: mygame.pck 4194304 bytes
  [godotpcktool] Files to zip: 42
  [godotpcktool] Zip ready: mygame.zip 3145728 bytes
  ```
  In `console.error` pane (Emscripten stderr):
  ```
  [godotpcktool] extractPck: 4194304 bytes received
  [godotpcktool] Written to /input.pck
  [godotpcktool] Loaded: Godot 4.3.0, format version 2
  [godotpcktool] Extract() complete
  [godotpcktool] 42 files extracted
  ```

- [ ] **Step 3: Test error path**

  Drop a non-PCK file (e.g., a `.txt` or image).
  Expected: red error message "Failed to load PCK. The file may be invalid…"
  Console: `[godotpcktool] ERROR: Load() failed`

- [ ] **Step 4: Test WASM module reuse**

  Drop a second `.pck` file without reloading the page.
  Expected: works correctly, no "Loading WASM module..." log a second time.

- [ ] **Step 5: Commit built artifacts (optional)**

  ```bash
  git add web/godotpcktool.js web/godotpcktool.wasm
  git commit -m "web: add compiled WASM artifacts"
  ```

  > **Note:** If you prefer not to commit binary artifacts (the `.wasm` file can be 1–2 MB), add `web/godotpcktool.js` and `web/godotpcktool.wasm` to `.gitignore` and document that users must run `make install-wasm` before serving.

---

## Spec Coverage Checklist

| Spec requirement | Task |
|---|---|
| User drops .pck, gets .zip | Tasks 3, 5 |
| Client-side only, static hosting | Task 2 (WASM), Task 5 (no server calls) |
| Lazy WASM load on first file drop | Task 5 (`getWasmModule()` called inside `extractAndZip`) |
| Zero changes to existing src/ besides guard option | Task 1 (option only, default ON) |
| Logging to console | Task 3 (`fprintf(stderr,...)`), Task 5 (`console.log/error`) |
| Error: invalid PCK | Tasks 3 (return 2) + 5 (red status) |
| Error: WASM fetch fail | Task 5 (`loadScript` catch) |
| Error: OOM | Task 2 (`ALLOW_MEMORY_GROWTH`) |
| Error: zero files | Task 3 (return 4) + 5 (propagated via `getLastError`) |
| Makefile targets: compile-wasm, install-wasm | Task 4 |
| Include.h generated for WASM build | Task 2 |
| WASM module cached between calls | Task 5 (`wasmModule` variable) |
| MEMFS cleaned between calls | Task 3 (`remove_all("/out/")`) |
| No user content in innerHTML | Task 5 (`setStatus` uses `textContent`, spinner uses DOM API) |
