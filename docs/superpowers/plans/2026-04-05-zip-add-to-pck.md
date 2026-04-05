# ZIP-to-PCK Batch Add — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Allow users to add all files from a `.zip` archive into an existing `.pck` in one operation, instead of adding files one by one.

**Architecture:** JS (already has JSZip) decompresses the ZIP, writes each file's bytes into WASM MEMFS at `/add_staged/<index>`, then calls a new C++ function `addMultipleFilesToPck` that reads a newline-separated manifest (`memfs_path\tpck_path`) and calls `AddSingleFile` for each entry before saving the PCK once. The "Add file" mode UI gains a second form row (strip-prefix field) shown only when a ZIP is selected. The single-file path in `runAdd()` is preserved unchanged.

**Tech Stack:** C++17 / Emscripten WASM, Vanilla JS, JSZip 3.x (already lazy-loaded).

---

## File Structure

| File | Change |
|---|---|
| `web/wasm_entry.cpp` | Add `addMultipleFilesToPck`; add `#include <sstream>` |
| `web/CMakeLists.txt` | Add `_addMultipleFilesToPck` to `EXPORTED_FUNCTIONS` |
| `web/index.html` | New HTML row, new DOM refs, new state var, updated `switchMode` / `updateAddStatus` / add-file zone wiring, ZIP branch in `runAdd` |

---

### Task 1: Add `addMultipleFilesToPck` to C++ and rebuild WASM

**Files:**
- Modify: `web/wasm_entry.cpp` — add `#include <sstream>` and new function
- Modify: `web/CMakeLists.txt` — export new function

- [ ] **Step 1: Add `#include <sstream>` to `wasm_entry.cpp`**

At the top of `web/wasm_entry.cpp`, the includes currently are:

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
```

Add `<sstream>` after `<regex>`:

```cpp
#include "pck/PckFile.h"
#include "FileFilter.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
```

- [ ] **Step 2: Add `addMultipleFilesToPck` function**

In `web/wasm_entry.cpp`, inside `extern "C" { }`, append the following function after the closing brace of `addToPck` (before `} // extern "C"`):

```cpp
// Loads PCK, adds multiple pre-staged files from WASM MEMFS, saves to /output.pck.
// manifest: newline-separated "memfs_path\tpck_path" pairs.
// JS must write each file to MEMFS (e.g. /add_staged/0, /add_staged/1) before calling this.
// Returns 0 on success, non-zero on error (call getLastError() for message).
int addMultipleFilesToPck(const uint8_t* pck_data, std::size_t pck_len,
                          const char* manifest)
{
    s_lastError.clear();

    fprintf(stderr, "[godotpcktool] addMultipleFilesToPck: pck=%zu bytes\n", pck_len);

    std::error_code ec;
    std::filesystem::remove("/input.pck", ec);
    std::filesystem::remove("/output.pck", ec);

    if(!writeToMemfs(pck_data, pck_len, "/input.pck")) {
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
    fprintf(stderr, "[godotpcktool] addMultipleFilesToPck: Godot %s, format version %u\n",
        pck.GetGodotVersion().c_str(), pck.GetFormatVersion());

    std::istringstream ss(manifest ? manifest : "");
    std::string line;
    int count = 0;
    while(std::getline(ss, line)) {
        if(line.empty())
            continue;
        const auto tab = line.find('\t');
        if(tab == std::string::npos) {
            s_lastError = "Malformed manifest line (missing tab): " + line;
            fprintf(stderr, "[godotpcktool] ERROR: %s\n", s_lastError.c_str());
            return 1;
        }
        const std::string memfsPath = line.substr(0, tab);
        const std::string pckPath   = line.substr(tab + 1);
        fprintf(stderr, "[godotpcktool] adding %s -> %s\n",
            memfsPath.c_str(), pckPath.c_str());
        try {
            pck.AddSingleFile(memfsPath, pckPath, false);
        } catch(const std::exception& e) {
            s_lastError = std::string("Failed to add ") + memfsPath + ": " + e.what();
            fprintf(stderr, "[godotpcktool] ERROR: %s\n", s_lastError.c_str());
            return 1;
        }
        ++count;
    }

    if(count == 0) {
        s_lastError = "Manifest was empty — no files to add.";
        fprintf(stderr, "[godotpcktool] ERROR: empty manifest\n");
        return 1;
    }

    pck.ChangePath("/output.pck");
    if(!pck.Save()) {
        s_lastError = "Failed to save modified PCK.";
        fprintf(stderr, "[godotpcktool] ERROR: Save() failed\n");
        return 3;
    }

    fprintf(stderr, "[godotpcktool] addMultipleFilesToPck: added %d files\n", count);
    return 0;
}
```

- [ ] **Step 3: Export the new function in `web/CMakeLists.txt`**

Find the `EXPORTED_FUNCTIONS` line (line 45):

```cmake
    "SHELL:-sEXPORTED_FUNCTIONS=['_extractPck','_getExtractedFiles','_getLastError','_listPck','_repackPck','_addToPck','_malloc','_free']"
```

Replace it with:

```cmake
    "SHELL:-sEXPORTED_FUNCTIONS=['_extractPck','_getExtractedFiles','_getLastError','_listPck','_repackPck','_addToPck','_addMultipleFilesToPck','_malloc','_free']"
```

- [ ] **Step 4: Rebuild WASM**

```bash
emcmake cmake -B build-wasm web/ && cmake --build build-wasm
```

Expected: build succeeds, `build-wasm/godotpcktool.js` and `build-wasm/godotpcktool.wasm` updated.

- [ ] **Step 5: Copy build artifacts into `web/`**

```bash
cp build-wasm/godotpcktool.js web/godotpcktool.js
cp build-wasm/godotpcktool.wasm web/godotpcktool.wasm
```

- [ ] **Step 6: Commit**

```bash
git add web/wasm_entry.cpp web/CMakeLists.txt web/godotpcktool.js web/godotpcktool.wasm
git commit -m "feat: add addMultipleFilesToPck WASM function for ZIP batch add"
```

---

### Task 2: Update `web/index.html` — HTML, DOM refs, state, mode switching

**Files:**
- Modify: `web/index.html`

- [ ] **Step 1: Add the ZIP strip-prefix form row to HTML**

Find the existing add-controls block (lines ~265-275):

```html
    <!-- Add mode controls -->
    <div id="add-controls" style="display:none">
      <div id="add-file-zone">
        Choose file to add
        <input type="file" id="add-file-input">
      </div>
      <div class="form-row">
        <label for="pck-path">Destination path inside the PCK</label>
        <input type="text" id="pck-path" placeholder="res://filename.png">
      </div>
      <button id="add-btn" style="display:none">Add to PCK</button>
    </div>
```

Replace it with:

```html
    <!-- Add mode controls -->
    <div id="add-controls" style="display:none">
      <div id="add-file-zone">
        Choose file to add (.zip or any file)
        <input type="file" id="add-file-input">
      </div>
      <div class="form-row" id="add-single-row">
        <label for="pck-path">Destination path inside the PCK</label>
        <input type="text" id="pck-path" placeholder="res://filename.png">
      </div>
      <div class="form-row" id="add-zip-row" style="display:none">
        <label for="zip-strip-prefix">Strip prefix from ZIP paths <span class="hint">(e.g. "assets" turns assets/hero.png → res://hero.png; leave empty to keep full path)</span></label>
        <input type="text" id="zip-strip-prefix" placeholder="auto-detected">
      </div>
      <button id="add-btn" style="display:none">Add to PCK</button>
    </div>
```

- [ ] **Step 2: Add new DOM refs**

Find the DOM refs block (around line 304). The current last few lines are:

```js
    const addBtn         = document.getElementById('add-btn');
    const statusEl       = document.getElementById('status');
    const downloadBtn    = document.getElementById('download-btn');
    const listOutput     = document.getElementById('list-output');
    const fileTree       = document.getElementById('file-tree');
```

Add three new refs after `addBtn`:

```js
    const addBtn         = document.getElementById('add-btn');
    const addSingleRow   = document.getElementById('add-single-row');
    const addZipRow      = document.getElementById('add-zip-row');
    const zipStripPrefix = document.getElementById('zip-strip-prefix');
    const statusEl       = document.getElementById('status');
    const downloadBtn    = document.getElementById('download-btn');
    const listOutput     = document.getElementById('list-output');
    const fileTree       = document.getElementById('file-tree');
```

- [ ] **Step 3: Add `selectedAddFileIsZip` state variable**

Find the State block (around line 296). Current last lines:

```js
    let selectedPckFile = null;  // add mode: the .pck to modify
    let selectedAddFile = null;  // add mode: the file to insert
    let activeMode      = 'extract';
```

Add one variable:

```js
    let selectedPckFile      = null;  // add mode: the .pck to modify
    let selectedAddFile      = null;  // add mode: the file to insert
    let selectedAddFileIsZip = false; // add mode: whether selected add-file is a ZIP
    let activeMode           = 'extract';
```

- [ ] **Step 4: Update `switchMode()` to reset ZIP state**

Find `switchMode` (around line 360). Current body ends with:

```js
      selectedPckFile = null;
      selectedAddFile = null;
      addBtn.style.display = 'none';
      pckPathInput.value   = '';
    }
```

Replace those last lines with:

```js
      selectedPckFile          = null;
      selectedAddFile          = null;
      selectedAddFileIsZip     = false;
      addBtn.style.display     = 'none';
      pckPathInput.value       = '';
      addSingleRow.style.display = 'block';
      addZipRow.style.display    = 'none';
      zipStripPrefix.value       = '';
    }
```

- [ ] **Step 5: Update `updateAddStatus()` to handle ZIP state**

Find the current `updateAddStatus` function (around line 678):

```js
    function updateAddStatus() {
      if (!selectedPckFile) {
        setStatus('Drop a .pck file in the zone above to get started.', '');
        addBtn.style.display = 'none';
      } else if (!selectedAddFile) {
        setStatus('PCK ready: ' + selectedPckFile.name + '. Now choose the file to add below.', '');
        addBtn.style.display = 'none';
      } else {
        setStatus('Ready: add ' + selectedAddFile.name + ' into ' + selectedPckFile.name, 'success');
        addBtn.style.display = 'block';
      }
    }
```

Replace it with:

```js
    function updateAddStatus() {
      if (!selectedPckFile) {
        setStatus('Drop a .pck file in the zone above to get started.', '');
        addBtn.style.display = 'none';
      } else if (!selectedAddFile) {
        setStatus('PCK ready: ' + selectedPckFile.name + '. Now choose the file to add below.', '');
        addBtn.style.display = 'none';
      } else if (selectedAddFileIsZip) {
        setStatus('ZIP ready: ' + selectedAddFile.name + '. Click \u201cAdd to PCK\u201d to add all files.', 'success');
        addBtn.style.display = 'block';
      } else {
        setStatus('Ready: add ' + selectedAddFile.name + ' into ' + selectedPckFile.name, 'success');
        addBtn.style.display = 'block';
      }
    }
```

- [ ] **Step 6: Update add-file zone wiring to detect ZIP**

Find the add-file zone wiring section (around line 766). Currently the `addFileInput` change handler and drop handler both contain:

```js
      selectedAddFile = f;
      if (!pckPathInput.value) pckPathInput.value = 'res://' + f.name;
      updateAddStatus();
```

Replace that pattern in **both** the `change` handler and the `drop` handler with:

```js
      selectedAddFile      = f;
      selectedAddFileIsZip = f.name.toLowerCase().endsWith('.zip') ||
                             f.type === 'application/zip';
      if (selectedAddFileIsZip) {
        addSingleRow.style.display = 'none';
        addZipRow.style.display    = 'block';
        zipStripPrefix.value       = '';
      } else {
        addSingleRow.style.display = 'block';
        addZipRow.style.display    = 'none';
        if (!pckPathInput.value) pckPathInput.value = 'res://' + f.name;
      }
      updateAddStatus();
```

- [ ] **Step 7: Commit HTML/JS structural changes**

```bash
git add web/index.html
git commit -m "feat: add ZIP UI row and state management in Add mode"
```

---

### Task 3: Implement `runAdd()` ZIP branch in `web/index.html`

**Files:**
- Modify: `web/index.html`

- [ ] **Step 1: Restructure `runAdd()` to branch on ZIP vs single-file**

Find the full `runAdd` function (around line 691). It currently looks like:

```js
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

        setStatusSpinner('Adding ' + selectedAddFile.name + ' to ' + selectedPckFile.name + '\u2026');

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
        const result  = Module._addToPck(pckPtr, pckBytes.length, filePtr, fileBytes.length, pathPtr);
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
```

Replace the entire `runAdd` function with this version that branches on `selectedAddFileIsZip`:

```js
    async function runAdd() {
      if (isProcessing) {
        setStatus('Please wait for the current operation to finish.', 'error');
        return;
      }
      if (!selectedPckFile || !selectedAddFile) {
        setStatus('Select both files first.', 'error');
        return;
      }
      if (!selectedAddFileIsZip) {
        const pckPath = pckPathInput.value.trim();
        if (!pckPath) {
          setStatus('Enter a destination path inside the PCK (e.g. res://myfile.png).', 'error');
          return;
        }
      }

      isProcessing = true;
      try {
        hideDownload();

        if (selectedAddFileIsZip) {
          // ── ZIP branch ──────────────────────────────────────────────────────
          setStatusSpinner('Loading tools\u2026');

          let Module, JSZip;
          try {
            [Module, JSZip] = await Promise.all([getWasmModule(), getJSZip()]);
          } catch (e) {
            setStatus('Failed to load tools: ' + e.message, 'error');
            return;
          }

          setStatusSpinner('Reading ZIP\u2026');

          let zipBytes;
          try {
            zipBytes = await selectedAddFile.arrayBuffer();
          } catch (e) {
            setStatus('Failed to read ZIP: ' + e.message, 'error');
            return;
          }

          let zip;
          try {
            zip = await new JSZip().loadAsync(zipBytes);
          } catch (e) {
            setStatus('Failed to parse ZIP: ' + e.message, 'error');
            return;
          }

          // Collect non-directory entries
          const entries = [];
          zip.forEach((relPath, file) => {
            if (!file.dir) entries.push({ relPath, file });
          });

          if (entries.length === 0) {
            setStatus('ZIP contains no files.', 'error');
            return;
          }

          // Auto-detect strip prefix: common top-level folder across all entries
          let stripPrefix = zipStripPrefix.value.trim();
          if (!stripPrefix) {
            const firstSegment = entries[0].relPath.split('/')[0];
            const allSharePrefix = entries.every(e => e.relPath.split('/')[0] === firstSegment);
            const anyHasSubdir   = entries.some(e => e.relPath.includes('/'));
            if (allSharePrefix && anyHasSubdir) {
              stripPrefix          = firstSegment;
              zipStripPrefix.value = stripPrefix;
            }
          }

          setStatusSpinner('Staging ' + entries.length + ' files\u2026');

          // Ensure /add_staged/ exists in MEMFS
          try { Module.FS.mkdir('/add_staged'); } catch (_) { /* already exists */ }

          const manifestLines = [];
          for (let i = 0; i < entries.length; i++) {
            const { relPath, file } = entries[i];
            let pckRelPath = relPath;
            if (stripPrefix && pckRelPath.startsWith(stripPrefix + '/')) {
              pckRelPath = pckRelPath.slice(stripPrefix.length + 1);
            }
            const pckPath   = pckRelPath.startsWith('res://') ? pckRelPath : 'res://' + pckRelPath;
            const memfsPath = '/add_staged/' + i;

            let bytes;
            try {
              bytes = await file.async('uint8array');
            } catch (e) {
              setStatus('Failed to read ' + relPath + ' from ZIP: ' + e.message, 'error');
              return;
            }

            Module.FS.writeFile(memfsPath, bytes);
            manifestLines.push(memfsPath + '\t' + pckPath);
          }

          const manifest = manifestLines.join('\n');
          setStatusSpinner('Adding ' + entries.length + ' files to ' + selectedPckFile.name + '\u2026');

          let pckBytes;
          try {
            pckBytes = new Uint8Array(await selectedPckFile.arrayBuffer());
          } catch (e) {
            setStatus('Failed to read PCK: ' + e.message, 'error');
            return;
          }

          console.log('[godotpcktool] Calling addMultipleFilesToPck:',
            selectedPckFile.name, pckBytes.length, 'bytes +',
            entries.length, 'files from', selectedAddFile.name);

          const pckPtr      = Module._malloc(pckBytes.length);
          Module.HEAPU8.set(pckBytes, pckPtr);
          const manifestPtr = allocString(Module, manifest);
          const result      = Module._addMultipleFilesToPck(pckPtr, pckBytes.length, manifestPtr);
          Module._free(pckPtr);
          Module._free(manifestPtr);

          if (result !== 0) {
            const err = Module.UTF8ToString(Module._getLastError());
            console.error('[godotpcktool] addMultipleFilesToPck error:', err);
            setStatus(err, 'error');
            return;
          }

          const outBytes = Module.FS.readFile('/output.pck');
          const blob     = new Blob([outBytes], { type: 'application/octet-stream' });
          const outName  = selectedPckFile.name.replace(/\.pck$/i, '') + '-modified.pck';
          const word     = entries.length === 1 ? 'file' : 'files';
          console.log('[godotpcktool] ZIP add done:', outName, blob.size, 'bytes');
          setStatus('Added ' + entries.length + ' ' + word + ' from ' + selectedAddFile.name, 'success');
          showDownload(blob, outName);

        } else {
          // ── Single-file branch (unchanged) ──────────────────────────────────
          const pckPath = pckPathInput.value.trim();
          setStatusSpinner('Loading extractor\u2026');

          let Module;
          try {
            Module = await getWasmModule();
          } catch (e) {
            setStatus('Failed to load extractor: ' + e.message, 'error');
            return;
          }

          setStatusSpinner('Adding ' + selectedAddFile.name + ' to ' + selectedPckFile.name + '\u2026');

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
          const result  = Module._addToPck(pckPtr, pckBytes.length, filePtr, fileBytes.length, pathPtr);
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
        }
      } finally {
        isProcessing = false;
      }
    }
```

- [ ] **Step 2: Commit**

```bash
git add web/index.html
git commit -m "feat: implement ZIP batch add in Add mode"
```

---

### Task 4: Browser verification

- [ ] **Step 1: Start local server**

```bash
python3 -m http.server 8080 --directory web/
```

Open `http://localhost:8080`.

- [ ] **Step 2: Verify single-file add still works**

1. Switch to **Add file** tab.
2. Drop a `.pck` onto the main drop zone.
3. Drop a single non-ZIP file onto the "Choose file to add" zone.
4. Confirm "Destination path inside the PCK" field is visible, "Strip prefix" field is hidden.
5. Confirm `pck-path` is pre-filled with `res://<filename>`.
6. Click "Add to PCK". Confirm download link appears and the status says "Added `<file>` to `<pck>`".

- [ ] **Step 3: Verify ZIP add — auto-detected prefix**

Prepare a ZIP whose contents are `assets/sprites/hero.png` and `assets/audio/theme.ogg` (i.e. all entries share `assets/` as top-level folder).

1. Drop the `.pck` onto the main drop zone.
2. Drop the `.zip` onto the "Choose file to add" zone.
3. Confirm "Strip prefix from ZIP paths" field appears (single-path field hidden).
4. Confirm `zip-strip-prefix` is auto-filled with `assets` after clicking "Add to PCK".
5. Download the modified `.pck`. Verify with the **List** tab that it contains `res://sprites/hero.png` and `res://audio/theme.ogg` (not `res://assets/...`).

- [ ] **Step 4: Verify ZIP add — manual prefix override**

1. Same ZIP as above. Before clicking "Add to PCK", clear the auto-detected prefix and type `assets/sprites`.
2. Click "Add to PCK". Verify `res://hero.png` is in the PCK (double-stripped).

- [ ] **Step 5: Verify ZIP add — no common prefix**

Prepare a ZIP whose entries are at the root: `hero.png`, `theme.ogg` (no shared top-level folder).

1. Drop and add as above.
2. Confirm `zip-strip-prefix` stays empty (no auto-detection).
3. Verify resulting PCK contains `res://hero.png` and `res://theme.ogg`.

- [ ] **Step 6: Verify ZIP with `res://` paths already inside**

Prepare a ZIP containing `res://sprites/hero.png` (the full Godot path as the ZIP entry name).

1. Add to PCK without any strip prefix.
2. Verify resulting PCK contains `res://sprites/hero.png` (not `res://res://sprites/hero.png`).

- [ ] **Step 7: Verify mode switch resets ZIP state**

1. Select a ZIP in Add mode (strip-prefix field visible).
2. Switch to Extract tab, then back to Add tab.
3. Confirm single-path field is visible again, strip-prefix field hidden, both inputs empty.

- [ ] **Step 8: Push to fork**

```bash
git push origin master
```
