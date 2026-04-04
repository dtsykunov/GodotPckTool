# WASM Browser Full CLI Extension Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Extend the browser PCK tool with list, repack, and add modes alongside the existing extract mode.

**Architecture:** Three new C functions (`listPck`, `repackPck`, `addToPck`) are added to `wasm_entry.cpp`, calling `pcktool::PckFile` directly. `index.html` gains a mode `<select>`, a per-mode description panel, per-mode controls, and JS flows for each mode. No files outside `web/` change.

**Tech Stack:** C++17, Emscripten WASM, vanilla JS (ES2017), JSZip CDN (extract mode only)

---

### Task 1: Update CMakeLists.txt exports

**Files:**
- Modify: `web/CMakeLists.txt`

- [ ] **Step 1: Read current CMakeLists.txt**

```bash
cat web/CMakeLists.txt
```

- [ ] **Step 2: Replace the EXPORTED_FUNCTIONS and EXPORTED_RUNTIME_METHODS lines**

Find this block (lines 44-48):
```cmake
target_link_options(godotpcktool PRIVATE
    "SHELL:-sEXPORTED_FUNCTIONS=['_extractPck','_getExtractedFiles','_getLastError','_malloc','_free']"
    "SHELL:-sEXPORTED_RUNTIME_METHODS=['FS','HEAPU8','UTF8ToString']"
    -sALLOW_MEMORY_GROWTH=1
    -sMODULARIZE=1
    -sEXPORT_NAME=GodotPckTool
)
```

Replace with:
```cmake
target_link_options(godotpcktool PRIVATE
    "SHELL:-sEXPORTED_FUNCTIONS=['_extractPck','_getExtractedFiles','_getLastError','_listPck','_repackPck','_addToPck','_malloc','_free']"
    "SHELL:-sEXPORTED_RUNTIME_METHODS=['FS','HEAPU8','UTF8ToString','stringToUTF8','lengthBytesUTF8']"
    -sALLOW_MEMORY_GROWTH=1
    -sMODULARIZE=1
    -sEXPORT_NAME=GodotPckTool
)
```

`stringToUTF8` and `lengthBytesUTF8` are needed so JS can write regex and path strings into the WASM heap.

- [ ] **Step 3: Commit**

```bash
git add web/CMakeLists.txt
git commit -m "build(wasm): export listPck, repackPck, addToPck and string helpers"
```

---

### Task 2: Extend wasm_entry.cpp with three new C functions

**Files:**
- Modify: `web/wasm_entry.cpp`

**Context:** `src/pck/PckFile.h` defines `pcktool::PckFile` with `Load()`, `Extract()`, `Save()`, `ChangePath()`, `AddSingleFile()`, `PreparePckPath()`, `SetIncludeFilter()`. `src/FileFilter.h` defines `pcktool::FileFilter` with `SetIncludeRegexes()`, `SetExcludeRegexes()`, `Include()`. The filter **must** be set before `Load()`.

- [ ] **Step 1: Replace the entire contents of web/wasm_entry.cpp**

```cpp
#include "pck/PckFile.h"
#include "FileFilter.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <regex>
#include <string>

static std::string s_manifest;
static std::string s_lastError;

// Walk dir recursively, appending relative paths to out.
static void walkDir(const std::filesystem::path& dir, std::size_t rootLen, std::string& out)
{
    for(const auto& entry : std::filesystem::recursive_directory_iterator(dir)) {
        if(!entry.is_regular_file())
            continue;
        std::string full = entry.path().string();
        std::string rel  = full.substr(rootLen);
        if(!rel.empty() && rel.front() == '/')
            rel.erase(rel.begin());
        if(!out.empty())
            out += '\n';
        out += rel;
    }
}

// Write bytes to a MEMFS path. Sets s_lastError and returns false on failure.
static bool writeToMemfs(const uint8_t* data, std::size_t len, const std::string& path)
{
    std::ofstream f(path, std::ios::binary);
    if(!f) {
        s_lastError = "Failed to open " + path + " for writing in virtual filesystem";
        return false;
    }
    f.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(len));
    if(!f.good()) {
        s_lastError = "Failed to write data to " + path + " in virtual filesystem";
        return false;
    }
    return true;
}

extern "C" {

int extractPck(const uint8_t* data, std::size_t len)
{
    s_manifest.clear();
    s_lastError.clear();

    fprintf(stderr, "[godotpcktool] extractPck: %zu bytes received\n", len);

    std::error_code ec;
    std::filesystem::remove_all("/out/", ec);
    std::filesystem::remove("/input.pck", ec);

    if(!writeToMemfs(data, len, "/input.pck")) {
        fprintf(stderr, "[godotpcktool] ERROR: %s\n", s_lastError.c_str());
        return 1;
    }
    fprintf(stderr, "[godotpcktool] Written to /input.pck\n");

    pcktool::PckFile pck("/input.pck");
    if(!pck.Load()) {
        s_lastError =
            "Failed to load PCK. The file may be invalid, corrupted, or an unsupported version.";
        fprintf(stderr, "[godotpcktool] ERROR: Load() failed\n");
        return 2;
    }
    fprintf(stderr, "[godotpcktool] Loaded: Godot %s, format version %u\n",
        pck.GetGodotVersion().c_str(), pck.GetFormatVersion());

    if(!pck.Extract("/out/", false)) {
        s_lastError = "Extraction failed. The PCK may be encrypted or contain unsupported data.";
        fprintf(stderr, "[godotpcktool] ERROR: Extract() failed\n");
        return 3;
    }
    fprintf(stderr, "[godotpcktool] Extract() complete\n");

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

// Returns newline-separated "path\tsize_in_bytes" manifest, or nullptr on error.
const char* listPck(const uint8_t* data, std::size_t len)
{
    s_manifest.clear();
    s_lastError.clear();

    fprintf(stderr, "[godotpcktool] listPck: %zu bytes received\n", len);

    std::error_code ec;
    std::filesystem::remove_all("/out/", ec);
    std::filesystem::remove("/input.pck", ec);

    if(!writeToMemfs(data, len, "/input.pck")) {
        fprintf(stderr, "[godotpcktool] ERROR: %s\n", s_lastError.c_str());
        return nullptr;
    }

    pcktool::PckFile pck("/input.pck");
    if(!pck.Load()) {
        s_lastError =
            "Failed to load PCK. The file may be invalid, corrupted, or an unsupported version.";
        fprintf(stderr, "[godotpcktool] ERROR: Load() failed\n");
        return nullptr;
    }
    fprintf(stderr, "[godotpcktool] listPck: Godot %s, format version %u\n",
        pck.GetGodotVersion().c_str(), pck.GetFormatVersion());

    if(!pck.Extract("/out/", false)) {
        s_lastError = "Extraction failed. The PCK may be encrypted or contain unsupported data.";
        fprintf(stderr, "[godotpcktool] ERROR: Extract() failed\n");
        return nullptr;
    }

    std::string root("/out/");
    for(const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        if(!entry.is_regular_file())
            continue;
        std::string full = entry.path().string();
        std::string rel  = full.substr(root.size());
        if(!rel.empty() && rel.front() == '/')
            rel.erase(rel.begin());
        const auto sz = static_cast<uint64_t>(entry.file_size());
        if(!s_manifest.empty())
            s_manifest += '\n';
        s_manifest += rel + '\t' + std::to_string(sz);
    }

    const int count = s_manifest.empty() ?
        0 :
        static_cast<int>(std::count(s_manifest.begin(), s_manifest.end(), '\n')) + 1;
    fprintf(stderr, "[godotpcktool] listPck: %d files\n", count);

    if(s_manifest.empty()) {
        s_lastError = "No files found in this PCK.";
        return nullptr;
    }

    return s_manifest.c_str();
}

// Loads PCK with optional regex filters, saves repacked result to /output.pck.
// include_regex / exclude_regex: empty string or null = no filter.
// Returns 0 on success, non-zero on error (call getLastError() for message).
int repackPck(const uint8_t* data, std::size_t len,
              const char* include_regex, const char* exclude_regex)
{
    s_lastError.clear();

    fprintf(stderr, "[godotpcktool] repackPck: %zu bytes received\n", len);

    std::error_code ec;
    std::filesystem::remove("/input.pck", ec);
    std::filesystem::remove("/output.pck", ec);

    if(!writeToMemfs(data, len, "/input.pck")) {
        fprintf(stderr, "[godotpcktool] ERROR: %s\n", s_lastError.c_str());
        return 1;
    }

    pcktool::PckFile pck("/input.pck");

    const std::string inc = (include_regex && *include_regex) ? include_regex : "";
    const std::string exc = (exclude_regex && *exclude_regex) ? exclude_regex : "";

    if(!inc.empty() || !exc.empty()) {
        try {
            pcktool::FileFilter filter;
            if(!inc.empty())
                filter.SetIncludeRegexes({std::regex(inc)});
            if(!exc.empty())
                filter.SetExcludeRegexes({std::regex(exc)});
            pck.SetIncludeFilter([filter](const pcktool::PckFile::ContainedFile& f) {
                return filter.Include(f);
            });
        } catch(const std::regex_error& e) {
            s_lastError = std::string("Invalid regex: ") + e.what();
            fprintf(stderr, "[godotpcktool] ERROR: %s\n", s_lastError.c_str());
            return 1;
        }
    }

    if(!pck.Load()) {
        s_lastError =
            "Failed to load PCK. The file may be invalid, corrupted, or an unsupported version.";
        fprintf(stderr, "[godotpcktool] ERROR: Load() failed\n");
        return 2;
    }
    fprintf(stderr, "[godotpcktool] repackPck: Godot %s, format version %u\n",
        pck.GetGodotVersion().c_str(), pck.GetFormatVersion());

    pck.ChangePath("/output.pck");

    if(!pck.Save()) {
        s_lastError = "Failed to save repacked PCK.";
        fprintf(stderr, "[godotpcktool] ERROR: Save() failed\n");
        return 3;
    }

    fprintf(stderr, "[godotpcktool] repackPck: done\n");
    return 0;
}

// Loads PCK, adds file_data at pck_path, saves modified PCK to /output.pck.
// pck_path: destination path inside the archive, e.g. "res://sprites/hero.png"
// Returns 0 on success, non-zero on error.
int addToPck(const uint8_t* pck_data, std::size_t pck_len,
             const uint8_t* file_data, std::size_t file_len,
             const char* pck_path)
{
    s_lastError.clear();

    fprintf(stderr, "[godotpcktool] addToPck: pck=%zu bytes, file=%zu bytes, path=%s\n",
        pck_len, file_len, pck_path ? pck_path : "(null)");

    std::error_code ec;
    std::filesystem::remove("/input.pck", ec);
    std::filesystem::remove("/add_input", ec);
    std::filesystem::remove("/output.pck", ec);

    if(!writeToMemfs(pck_data, pck_len, "/input.pck")) {
        fprintf(stderr, "[godotpcktool] ERROR: %s\n", s_lastError.c_str());
        return 1;
    }
    if(!writeToMemfs(file_data, file_len, "/add_input")) {
        fprintf(stderr, "[godotpcktool] ERROR: %s\n", s_lastError.c_str());
        return 1;
    }

    pcktool::PckFile pck("/input.pck");
    if(!pck.Load()) {
        s_lastError =
            "Failed to load PCK. The file may be invalid, corrupted, or an unsupported version.";
        fprintf(stderr, "[godotpcktool] ERROR: Load() failed\n");
        return 2;
    }
    fprintf(stderr, "[godotpcktool] addToPck: Godot %s, format version %u\n",
        pck.GetGodotVersion().c_str(), pck.GetFormatVersion());

    pck.ChangePath("/output.pck");
    pck.AddSingleFile("/add_input",
        pck.PreparePckPath(pck_path ? pck_path : "", ""), false);

    if(!pck.Save()) {
        s_lastError = "Failed to save modified PCK.";
        fprintf(stderr, "[godotpcktool] ERROR: Save() failed\n");
        return 3;
    }

    fprintf(stderr, "[godotpcktool] addToPck: done\n");
    return 0;
}

} // extern "C"
```

- [ ] **Step 2: Build to verify no compile errors**

```bash
make compile-wasm 2>&1 | tail -20
```

Expected: build completes, no errors. Warnings about unused variables or includes are acceptable.

- [ ] **Step 3: Verify new symbols appear in the output**

```bash
ls -lh build-wasm/godotpcktool.js build-wasm/godotpcktool.wasm
grep -o '"_listPck"\|"_repackPck"\|"_addToPck"' build-wasm/godotpcktool.js | sort -u
```

Expected: all three symbol names found.

- [ ] **Step 4: Commit**

```bash
git add web/wasm_entry.cpp
git commit -m "feat(wasm): add listPck, repackPck, addToPck C entry points"
```

---

### Task 3: Rewrite index.html with all four modes

**Files:**
- Modify: `web/index.html`

**Context:** The existing file is ~264 lines. This task replaces it entirely. Key patterns to preserve: `setStatus`/`setStatusSpinner` use `textContent` only (XSS safety), `isProcessing` guard, `activeBlobUrl` tracking, lazy `loadScript` loader, `getWasmModule`/`getJSZip` cache pattern. New: `handlePckFile(file)` dispatcher routes to the correct mode function.

- [ ] **Step 1: Replace the entire contents of web/index.html**

```html
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Godot PCK Tool</title>
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
    p.subtitle { color: #aaa; margin-bottom: 1.5rem; font-size: 0.95rem; text-align: center; }

    .mode-bar {
      width: 100%; max-width: 480px;
      margin-bottom: 0.75rem;
      display: flex; align-items: center; gap: 0.5rem;
    }
    .mode-bar label { color: #aaa; font-size: 0.9rem; white-space: nowrap; }
    .mode-bar select {
      background: #16213e; color: #e0e0e0;
      border: 1px solid #4a90d9; border-radius: 6px;
      padding: 0.35rem 0.6rem; font-size: 0.9rem;
      cursor: pointer; flex: 1;
    }

    #mode-desc {
      color: #bbb; font-size: 0.88rem; line-height: 1.55;
      margin-bottom: 1.25rem;
      width: 100%; max-width: 480px; text-align: center;
    }

    #drop-zone {
      border: 2px dashed #4a90d9; border-radius: 12px;
      padding: 3rem 4rem; text-align: center; cursor: pointer;
      transition: background 0.2s, border-color 0.2s;
      width: 100%; max-width: 480px;
    }
    #drop-zone.drag-over { background: #1e3a5f; border-color: #7ab8f5; }
    #drop-zone p { color: #aaa; margin-top: 0.5rem; font-size: 0.9rem; }
    #file-input { display: none; }

    .extra-controls { width: 100%; max-width: 480px; margin-top: 1rem; }
    .form-row { display: flex; flex-direction: column; gap: 0.3rem; margin-bottom: 0.75rem; }
    .form-row label { color: #aaa; font-size: 0.85rem; }
    .form-row .hint { color: #555; font-size: 0.8rem; }
    .form-row input[type="text"] {
      background: #16213e; color: #e0e0e0;
      border: 1px solid #4a90d9; border-radius: 6px;
      padding: 0.45rem 0.6rem; font-size: 0.9rem;
      font-family: monospace; width: 100%;
    }
    .form-row input[type="text"]::placeholder { color: #444; }

    #add-file-zone {
      border: 2px dashed #5a7a9d; border-radius: 10px;
      padding: 1.5rem; text-align: center; cursor: pointer;
      transition: background 0.2s, border-color 0.2s;
      margin-bottom: 0.75rem; color: #aaa; font-size: 0.9rem;
    }
    #add-file-zone.drag-over { background: #1e3a5f; border-color: #7ab8f5; }
    #add-file-input { display: none; }

    #add-btn {
      width: 100%; padding: 0.6rem;
      background: #2a6099; color: #fff;
      border: none; border-radius: 8px;
      font-size: 0.95rem; cursor: pointer; margin-top: 0.25rem;
    }
    #add-btn:hover { background: #357abd; }

    #status {
      margin-top: 1.5rem; min-height: 2rem;
      text-align: center; font-size: 0.95rem;
      width: 100%; max-width: 480px;
    }
    #status.error   { color: #ff6b6b; }
    #status.success { color: #6bffb8; }

    #download-btn {
      display: none; margin-top: 1rem;
      padding: 0.7rem 2rem; background: #4a90d9; color: #fff;
      border: none; border-radius: 8px; font-size: 1rem;
      cursor: pointer; text-decoration: none;
    }
    #download-btn:hover { background: #357abd; }

    #list-output { margin-top: 1.5rem; width: 100%; max-width: 700px; overflow-x: auto; }
    table { width: 100%; border-collapse: collapse; font-size: 0.88rem; }
    th {
      text-align: left; padding: 0.5rem 0.75rem;
      background: #16213e; color: #7ab8f5;
      border-bottom: 1px solid #2a4060;
    }
    td {
      padding: 0.4rem 0.75rem; border-bottom: 1px solid #1e2e44;
      font-family: monospace; word-break: break-all;
    }
    tr:nth-child(even) td { background: rgba(22,33,62,0.4); }

    .spinner {
      display: inline-block; width: 1em; height: 1em;
      border: 2px solid #4a90d9; border-top-color: transparent;
      border-radius: 50%; animation: spin 0.7s linear infinite;
      vertical-align: middle; margin-right: 0.4em;
    }
    @keyframes spin { to { transform: rotate(360deg); } }
  </style>
</head>
<body>
  <h1>Godot PCK Tool</h1>
  <p class="subtitle">Inspect, extract, repack, or modify Godot <code>.pck</code> archives — entirely in your browser, no server required</p>

  <div class="mode-bar">
    <label for="mode-select">Mode:</label>
    <select id="mode-select">
      <option value="extract">Extract → .zip</option>
      <option value="list">List contents</option>
      <option value="repack">Repack (filter)</option>
      <option value="add">Add file</option>
    </select>
  </div>

  <p id="mode-desc"></p>

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

  <!-- Repack mode controls -->
  <div id="repack-controls" class="extra-controls" style="display:none">
    <div class="form-row">
      <label for="include-regex">Include regex <span class="hint">(empty = include all files)</span></label>
      <input type="text" id="include-regex" placeholder="e.g. \.png$">
    </div>
    <div class="form-row">
      <label for="exclude-regex">Exclude regex <span class="hint">(empty = exclude nothing)</span></label>
      <input type="text" id="exclude-regex" placeholder="e.g. \.import$">
    </div>
  </div>

  <!-- Add mode controls -->
  <div id="add-controls" class="extra-controls" style="display:none">
    <div id="add-file-zone">
      Drop the file to add here, or click to select
      <input type="file" id="add-file-input">
    </div>
    <div class="form-row">
      <label for="pck-path">Destination path inside the PCK</label>
      <input type="text" id="pck-path" placeholder="res://filename.png">
    </div>
    <button id="add-btn" style="display:none">Add to PCK</button>
  </div>

  <div id="status"></div>
  <a id="download-btn" style="display:none">Download</a>

  <!-- List mode output -->
  <div id="list-output" style="display:none">
    <table id="file-table">
      <thead><tr><th>Path</th><th>Size</th></tr></thead>
      <tbody id="file-table-body"></tbody>
    </table>
  </div>

  <script>
    // ── Constants ─────────────────────────────────────────────────────────────
    const MODE_DESCS = {
      extract: 'Drop a .pck file to extract all its contents and download them as a .zip archive.',
      list:    'Drop a .pck file to inspect its contents. A table of file paths and sizes will appear below — nothing is downloaded.',
      repack:  'Drop a .pck file to repack it, optionally filtering files by regex. Leave both fields empty to repack as-is. The result downloads as a new .pck file.',
      add:     'Drop a .pck file above, then select the file to add using the picker below. Set its destination path inside the archive (e.g. res://sprites/hero.png) and click "Add to PCK". The modified .pck downloads.'
    };

    // ── State ────────────────────────────────────────────────────────────────
    let wasmModule    = null;   // cached Emscripten module instance
    let jszipLib      = null;   // cached JSZip class
    let activeBlobUrl = null;   // tracked so it can be revoked on next download
    let isProcessing  = false;  // guard against concurrent operations
    let selectedPckFile  = null; // add mode: the .pck to modify
    let selectedAddFile  = null; // add mode: the file to insert

    // ── DOM refs ──────────────────────────────────────────────────────────────
    const modeSelect     = document.getElementById('mode-select');
    const modeDesc       = document.getElementById('mode-desc');
    const dropZone       = document.getElementById('drop-zone');
    const fileInput      = document.getElementById('file-input');
    const repackControls = document.getElementById('repack-controls');
    const addControls    = document.getElementById('add-controls');
    const addFileZone    = document.getElementById('add-file-zone');
    const addFileInput   = document.getElementById('add-file-input');
    const pckPathInput   = document.getElementById('pck-path');
    const addBtn         = document.getElementById('add-btn');
    const statusEl       = document.getElementById('status');
    const downloadBtn    = document.getElementById('download-btn');
    const listOutput     = document.getElementById('list-output');
    const fileTableBody  = document.getElementById('file-table-body');

    // ── UI helpers ────────────────────────────────────────────────────────────
    // All user-controlled content goes through textContent, never innerHTML.
    function setStatus(message, cls) {
      statusEl.textContent = message;
      statusEl.className   = cls || '';
    }

    // Spinner uses only trusted static markup — no user content.
    function setStatusSpinner(staticMessage) {
      statusEl.className   = '';
      statusEl.textContent = '';
      const spinner = document.createElement('span');
      spinner.className = 'spinner';
      statusEl.appendChild(spinner);
      statusEl.appendChild(document.createTextNode(' ' + staticMessage));
    }

    function showDownload(blob, filename) {
      if (activeBlobUrl) URL.revokeObjectURL(activeBlobUrl);
      activeBlobUrl        = URL.createObjectURL(blob);
      downloadBtn.href     = activeBlobUrl;
      downloadBtn.download = filename;
      downloadBtn.textContent = 'Download ' + filename;
      downloadBtn.style.display = 'inline-block';
    }

    function hideDownload() {
      downloadBtn.style.display = 'none';
      downloadBtn.href = '';
    }

    function formatSize(bytes) {
      if (bytes >= 1024 * 1024) return (bytes / (1024 * 1024)).toFixed(1) + ' MB';
      if (bytes >= 1024)        return (bytes / 1024).toFixed(1) + ' KB';
      return bytes + ' B';
    }

    // ── Mode switching ────────────────────────────────────────────────────────
    function switchMode(mode) {
      modeDesc.textContent = MODE_DESCS[mode] || '';
      repackControls.style.display = mode === 'repack' ? 'block' : 'none';
      addControls.style.display    = mode === 'add'    ? 'block' : 'none';
      setStatus('', '');
      hideDownload();
      listOutput.style.display  = 'none';
      fileTableBody.textContent = '';
      selectedPckFile = null;
      selectedAddFile = null;
      addBtn.style.display = 'none';
      pckPathInput.value   = '';
    }

    modeSelect.addEventListener('change', () => switchMode(modeSelect.value));
    switchMode('extract'); // initialise description text

    // ── Lazy loaders ──────────────────────────────────────────────────────────
    function loadScript(src) {
      return new Promise((resolve, reject) => {
        const s = document.createElement('script');
        s.src    = src;
        s.onload = resolve;
        s.onerror = () => reject(new Error('Failed to load script: ' + src));
        document.head.appendChild(s);
      });
    }

    async function getWasmModule() {
      if (wasmModule) return wasmModule;
      console.log('[godotpcktool] Loading WASM module...');
      await loadScript('godotpcktool.js');
      // GodotPckTool is a factory function (MODULARIZE=1, EXPORT_NAME=GodotPckTool)
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

    // ── WASM string helper ────────────────────────────────────────────────────
    // Allocates a UTF-8 C string in the WASM heap. Caller must _free() the result.
    function allocString(Module, str) {
      const len = Module.lengthBytesUTF8(str) + 1;
      const ptr = Module._malloc(len);
      Module.stringToUTF8(str, ptr, len);
      return ptr;
    }

    // ── Mode: Extract ─────────────────────────────────────────────────────────
    async function extractAndZip(file) {
      if (isProcessing) {
        setStatus('Please wait for the current operation to finish.', 'error');
        return;
      }
      isProcessing = true;
      try {
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
          bytes = new Uint8Array(await file.arrayBuffer());
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
      } finally {
        isProcessing = false;
      }
    }

    // ── Mode: List ────────────────────────────────────────────────────────────
    async function listContents(file) {
      if (isProcessing) {
        setStatus('Please wait for the current operation to finish.', 'error');
        return;
      }
      isProcessing = true;
      try {
        hideDownload();
        listOutput.style.display  = 'none';
        fileTableBody.textContent = '';
        setStatusSpinner('Loading extractor\u2026');

        let Module;
        try {
          Module = await getWasmModule();
        } catch (e) {
          setStatus('Failed to load extractor: ' + e.message, 'error');
          return;
        }

        setStatusSpinner('Reading ' + file.name + '\u2026');

        let bytes;
        try {
          bytes = new Uint8Array(await file.arrayBuffer());
        } catch (e) {
          setStatus('Failed to read file: ' + e.message, 'error');
          return;
        }

        console.log('[godotpcktool] Calling listPck:', file.name, bytes.length, 'bytes');

        const ptr       = Module._malloc(bytes.length);
        Module.HEAPU8.set(bytes, ptr);
        const resultPtr = Module._listPck(ptr, bytes.length);
        Module._free(ptr);

        if (!resultPtr) {
          const err = Module.UTF8ToString(Module._getLastError());
          console.error('[godotpcktool] listPck error:', err);
          setStatus(err, 'error');
          return;
        }

        const manifestStr = Module.UTF8ToString(resultPtr);
        const lines = manifestStr.split('\n').filter(Boolean);
        console.log('[godotpcktool] Listed', lines.length, 'files');

        for (const line of lines) {
          const tab      = line.indexOf('\t');
          const path     = tab >= 0 ? line.slice(0, tab) : line;
          const sizeBytes = tab >= 0 ? parseInt(line.slice(tab + 1), 10) : 0;
          const tr       = document.createElement('tr');
          const tdPath   = document.createElement('td');
          tdPath.textContent = path;
          const tdSize   = document.createElement('td');
          tdSize.textContent = formatSize(sizeBytes);
          tr.appendChild(tdPath);
          tr.appendChild(tdSize);
          fileTableBody.appendChild(tr);
        }

        listOutput.style.display = 'block';
        const word = lines.length === 1 ? 'file' : 'files';
        setStatus(lines.length + ' ' + word + ' in ' + file.name, 'success');
      } finally {
        isProcessing = false;
      }
    }

    // ── Mode: Repack ──────────────────────────────────────────────────────────
    async function repackFile(file) {
      if (isProcessing) {
        setStatus('Please wait for the current operation to finish.', 'error');
        return;
      }
      isProcessing = true;
      try {
        hideDownload();
        setStatusSpinner('Loading extractor\u2026');

        let Module;
        try {
          Module = await getWasmModule();
        } catch (e) {
          setStatus('Failed to load extractor: ' + e.message, 'error');
          return;
        }

        const includeRe = document.getElementById('include-regex').value.trim();
        const excludeRe = document.getElementById('exclude-regex').value.trim();

        setStatusSpinner('Repacking ' + file.name + '\u2026');

        let bytes;
        try {
          bytes = new Uint8Array(await file.arrayBuffer());
        } catch (e) {
          setStatus('Failed to read file: ' + e.message, 'error');
          return;
        }

        console.log('[godotpcktool] Calling repackPck:', file.name, bytes.length, 'bytes',
          'include:', includeRe || '(none)', 'exclude:', excludeRe || '(none)');

        const ptr    = Module._malloc(bytes.length);
        Module.HEAPU8.set(bytes, ptr);
        const incPtr = allocString(Module, includeRe);
        const excPtr = allocString(Module, excludeRe);
        const result = Module._repackPck(ptr, bytes.length, incPtr, excPtr);
        Module._free(ptr);
        Module._free(incPtr);
        Module._free(excPtr);

        if (result !== 0) {
          const err = Module.UTF8ToString(Module._getLastError());
          console.error('[godotpcktool] repackPck error:', err);
          setStatus(err, 'error');
          return;
        }

        const outBytes = Module.FS.readFile('/output.pck');
        const blob     = new Blob([outBytes], { type: 'application/octet-stream' });
        const outName  = file.name.replace(/\.pck$/i, '') + '-repacked.pck';
        console.log('[godotpcktool] Repack done:', outName, blob.size, 'bytes');
        setStatus('Repacked ' + file.name + ' (' + formatSize(blob.size) + ')', 'success');
        showDownload(blob, outName);
      } finally {
        isProcessing = false;
      }
    }

    // ── Mode: Add ─────────────────────────────────────────────────────────────
    function updateAddStatus() {
      if (!selectedPckFile) {
        setStatus('Drop a .pck file in the zone above to get started.', '');
        addBtn.style.display = 'none';
      } else if (!selectedAddFile) {
        setStatus('PCK ready: ' + selectedPckFile.name + '. Now drop the file to add below.', '');
        addBtn.style.display = 'none';
      } else {
        setStatus('Ready: add ' + selectedAddFile.name + ' into ' + selectedPckFile.name, 'success');
        addBtn.style.display = 'block';
      }
    }

    async function runAdd() {
      if (isProcessing) {
        setStatus('Please wait for the current operation to finish.', 'error');
        return;
      }
      if (!selectedPckFile || !selectedAddFile) {
        setStatus('Select both files first.', 'error');
        return;
      }
      const pckPath = pckPathInput.value.trim();
      if (!pckPath) {
        setStatus('Enter a destination path inside the PCK (e.g. res://myfile.png).', 'error');
        return;
      }

      isProcessing = true;
      try {
        hideDownload();
        setStatusSpinner('Loading extractor\u2026');

        let Module;
        try {
          Module = await getWasmModule();
        } catch (e) {
          setStatus('Failed to load extractor: ' + e.message, 'error');
          return;
        }

        setStatusSpinner(
          'Adding ' + selectedAddFile.name + ' to ' + selectedPckFile.name + '\u2026'
        );

        let pckBytes, fileBytes;
        try {
          [pckBytes, fileBytes] = await Promise.all([
            selectedPckFile.arrayBuffer().then(b => new Uint8Array(b)),
            selectedAddFile.arrayBuffer().then(b => new Uint8Array(b))
          ]);
        } catch (e) {
          setStatus('Failed to read files: ' + e.message, 'error');
          return;
        }

        console.log('[godotpcktool] Calling addToPck:',
          selectedPckFile.name, pckBytes.length, '+',
          selectedAddFile.name, fileBytes.length, '->', pckPath);

        const pckPtr  = Module._malloc(pckBytes.length);
        Module.HEAPU8.set(pckBytes, pckPtr);
        const filePtr = Module._malloc(fileBytes.length);
        Module.HEAPU8.set(fileBytes, filePtr);
        const pathPtr = allocString(Module, pckPath);
        const result  = Module._addToPck(
          pckPtr, pckBytes.length, filePtr, fileBytes.length, pathPtr
        );
        Module._free(pckPtr);
        Module._free(filePtr);
        Module._free(pathPtr);

        if (result !== 0) {
          const err = Module.UTF8ToString(Module._getLastError());
          console.error('[godotpcktool] addToPck error:', err);
          setStatus(err, 'error');
          return;
        }

        const outBytes = Module.FS.readFile('/output.pck');
        const blob     = new Blob([outBytes], { type: 'application/octet-stream' });
        const outName  = selectedPckFile.name.replace(/\.pck$/i, '') + '-modified.pck';
        console.log('[godotpcktool] Add done:', outName, blob.size, 'bytes');
        setStatus('Added ' + selectedAddFile.name + ' to ' + selectedPckFile.name, 'success');
        showDownload(blob, outName);
      } finally {
        isProcessing = false;
      }
    }

    addBtn.addEventListener('click', runAdd);

    // ── Add file zone wiring ──────────────────────────────────────────────────
    addFileZone.addEventListener('click', () => addFileInput.click());
    addFileInput.addEventListener('change', () => {
      const f = addFileInput.files[0];
      if (!f) return;
      selectedAddFile = f;
      if (!pckPathInput.value) pckPathInput.value = 'res://' + f.name;
      updateAddStatus();
    });
    addFileZone.addEventListener('dragover', e => {
      e.preventDefault();
      addFileZone.classList.add('drag-over');
    });
    addFileZone.addEventListener('dragleave', () => {
      addFileZone.classList.remove('drag-over');
    });
    addFileZone.addEventListener('drop', e => {
      e.preventDefault();
      addFileZone.classList.remove('drag-over');
      const f = e.dataTransfer.files[0];
      if (!f) return;
      selectedAddFile = f;
      if (!pckPathInput.value) pckPathInput.value = 'res://' + f.name;
      updateAddStatus();
    });

    // ── Main PCK drop zone + file input ──────────────────────────────────────
    function handlePckFile(file) {
      const mode = modeSelect.value;
      if      (mode === 'extract') extractAndZip(file);
      else if (mode === 'list')    listContents(file);
      else if (mode === 'repack')  repackFile(file);
      else if (mode === 'add') {
        selectedPckFile = file;
        updateAddStatus();
      }
    }

    dropZone.addEventListener('click', () => fileInput.click());
    fileInput.addEventListener('change', () => {
      if (fileInput.files[0]) handlePckFile(fileInput.files[0]);
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
      if (file) handlePckFile(file);
    });
  </script>
</body>
</html>
```

- [ ] **Step 2: Commit**

```bash
git add web/index.html
git commit -m "feat(web): add list, repack, add modes with per-mode UI and descriptions"
```

---

### Task 4: Write web/README.md

**Files:**
- Create: `web/README.md`

- [ ] **Step 1: Create web/README.md**

```markdown
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
```

- [ ] **Step 2: Commit**

```bash
git add web/README.md
git commit -m "docs(web): add README covering all modes, build, and serve instructions"
```

---

### Task 5: Full build, install, and smoke test

**Files:** none (verification only)

- [ ] **Step 1: Do a clean build and install**

```bash
make install-wasm 2>&1 | tail -30
```

Expected: `godotpcktool.js` and `godotpcktool.wasm` copied into `web/`.

- [ ] **Step 2: Serve the web directory**

```bash
python3 -m http.server 8080 --directory web/ &
```

- [ ] **Step 3: Smoke test — List mode**

Open `http://localhost:8080`. Select "List contents". Drop any `.pck` file.
Expected: table of paths and sizes appears, no download button, status shows file count.

- [ ] **Step 4: Smoke test — Extract mode**

Select "Extract → .zip". Drop the same `.pck`.
Expected: "Download <name>.zip" button appears, clicking it downloads a valid zip.

- [ ] **Step 5: Smoke test — Repack mode**

Select "Repack". Drop the `.pck`. Leave regex fields empty. Drop the file.
Expected: "Download <name>-repacked.pck" appears. Optionally enter `\.png$` as include
regex, drop again — repacked file should be smaller and contain only `.png` files.

- [ ] **Step 6: Smoke test — Add mode**

Select "Add file". Drop the `.pck`. Drop any small file into the add zone.
Expected: path field pre-fills with `res://<filename>`, "Add to PCK" button appears.
Click it. Expected: "Download <name>-modified.pck" appears. Switch to List mode and
drop the modified `.pck` — the added file should appear in the table.

- [ ] **Step 7: Stop the server**

```bash
kill %1
```

- [ ] **Step 8: Final commit (if any files were modified during testing)**

```bash
git status
# if clean:
git log --oneline -6
```
