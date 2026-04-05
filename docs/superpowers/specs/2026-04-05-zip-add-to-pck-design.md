# ZIP-to-PCK Batch Add — Design Spec

**Date:** 2026-04-05
**Status:** Approved

## Goal

Allow users to add multiple files to a `.pck` archive at once by providing a `.zip` archive in the "Add file" mode of the browser tool. Previously only a single file could be added per operation.

## Architecture

All changes split between two files:

| File | Change |
|---|---|
| `web/wasm_entry.cpp` | New `addMultipleFilesToPck` C function |
| `web/CMakeLists.txt` | Export `addMultipleFilesToPck` |
| `web/index.html` | ZIP detection, JSZip unpacking, MEMFS staging, updated UI |

No new files. No changes to `src/`.

---

## C++ — `web/wasm_entry.cpp`

### New function: `addMultipleFilesToPck`

```cpp
int addMultipleFilesToPck(
    const uint8_t* pck_data, size_t pck_len,
    const char* manifest
)
```

**Manifest format:** newline-separated pairs, each line `memfs_path\tpck_path`. Example:

```
/add_staged/hero.png\tres://sprites/hero.png
/add_staged/bg.ogg\tres://audio/bg.ogg
```

**Behavior:**
1. Clear `s_lastError`.
2. Clean up `/add_staged/`, `/input.pck`, `/output.pck` from prior runs.
3. Write `pck_data` to `/input.pck` via `writeToMemfs`.
4. Load PCK with `PckFile::Load()`. On failure set `s_lastError`, return 2.
5. Parse manifest line by line. For each line, split on `\t` to get `memfs_path` and `pck_path`. Call `pck.AddSingleFile(memfs_path, pck_path, false)`. If any line is malformed, set `s_lastError`, return 1.
6. `pck.ChangePath("/output.pck")` then `pck.Save()`. On failure set `s_lastError`, return 3.
7. Return 0.

**CMakeLists.txt:** add `_addMultipleFilesToPck` to `EXPORTED_FUNCTIONS`.

---

## JavaScript / UI — `web/index.html`

### State

- `selectedAddFileIsZip` (boolean, default `false`) — tracks whether the chosen add-file is a ZIP archive.
- Reset to `false` in `switchMode()`.

### HTML changes (Add controls)

Replace the single "Destination path" form row with two mutually exclusive rows:

```html
<!-- shown when single file selected -->
<div class="form-row" id="add-single-row">
  <label for="pck-path">Destination path inside the PCK</label>
  <input type="text" id="pck-path" placeholder="res://filename.png">
</div>

<!-- shown when ZIP selected -->
<div class="form-row" id="add-zip-row" style="display:none">
  <label for="zip-strip-prefix">
    Strip prefix from ZIP paths
    <span class="hint">(e.g. "assets" turns assets/hero.png → res://hero.png; leave empty to use raw paths)</span>
  </label>
  <input type="text" id="zip-strip-prefix" placeholder="auto-detected">
</div>
```

`add-file-input` gains `accept` attribute listing common types plus `.zip` (no strict restriction — any file type is allowed for single-file add).

### `updateAddStatus()`

- No file selected → same as before.
- Single file selected → same as before.
- ZIP selected → "ZIP ready: `<name>` (`N` entries). Click 'Add to PCK' to proceed."

### `switchMode()`

- Reset `selectedAddFileIsZip = false`.
- Hide `add-zip-row`, show `add-single-row`.
- Clear `zip-strip-prefix` value.

### `runAdd()` — ZIP branch

Triggered when `selectedAddFileIsZip === true`:

1. Load WASM module and JSZip in parallel (`Promise.all`).
2. Read ZIP bytes from `selectedAddFile.arrayBuffer()`.
3. Use `JSZip.loadAsync(bytes)` to parse the archive.
4. Filter out directory entries (files whose name ends with `/`).
5. **Auto-detect strip prefix:** collect all file paths; if every path starts with the same first path segment, use that segment as the default. The `zip-strip-prefix` field is pre-filled with this value if it's currently empty.
6. For each file in the ZIP:
   - Compute `zipPath` = the file's path inside the archive.
   - Apply strip prefix: if `zipPath` starts with `<prefix>/`, remove that prefix; otherwise leave as-is.
   - Compute `pckPath` = `res://` + stripped path (unless it already starts with `res://`).
   - Compute `memfsPath` = `/add_staged/` + index (e.g. `/add_staged/0`, `/add_staged/1`) to avoid filename collisions.
   - Read file bytes via `file.async('uint8array')`.
   - Write to WASM MEMFS: `Module.FS.writeFile(memfsPath, bytes)`.
   - Append manifest line: `memfsPath + '\t' + pckPath`.
7. Allocate WASM heap buffers for PCK bytes and manifest string.
8. Call `Module._addMultipleFilesToPck(pckPtr, pckLen, manifestPtr)`.
9. Free heap buffers.
10. On success: read `/output.pck` from MEMFS, create Blob, offer download as `<pckName>-modified.pck`.
11. On failure: read `getLastError()`, show as error status.

### Add-file zone wiring

When a file is selected (via click or drag-drop on `add-file-zone`):
- Detect ZIP: `f.name.endsWith('.zip') || f.type === 'application/zip'`.
- If ZIP: set `selectedAddFileIsZip = true`, show `add-zip-row`, hide `add-single-row`. Auto-detect of the strip prefix is deferred to `runAdd` (async) — `zip-strip-prefix` starts empty and is filled just before the operation runs.
- If single file: set `selectedAddFileIsZip = false`, show `add-single-row`, hide `add-zip-row`, pre-fill `pck-path` with `res://` + filename (existing behavior).

---

## Path handling rules

| ZIP path | Strip prefix field | Resulting PCK path |
|---|---|---|
| `assets/sprites/hero.png` | `assets` | `res://sprites/hero.png` |
| `assets/sprites/hero.png` | _(empty)_ | `res://assets/sprites/hero.png` |
| `res://sprites/hero.png` | _(any)_ | `res://sprites/hero.png` _(already has res://)_ |
| `hero.png` | _(any)_ | `res://hero.png` |

---

## Error handling

| Scenario | Behavior |
|---|---|
| ZIP is empty | JS shows error before calling C++ |
| ZIP has only directory entries | JS shows error before calling C++ |
| Manifest line malformed | C++ sets `s_lastError`, returns 1 |
| PCK load fails | C++ sets `s_lastError`, returns 2 |
| PCK save fails | C++ sets `s_lastError`, returns 3 |
| JSZip fails to parse ZIP | JS catches exception, shows error |

---

## Out of scope

- Creating a new PCK from scratch (no existing `.pck` input).
- Nested ZIPs.
- ZIP encryption.
- Progress indicator per-file (single spinner for full operation is sufficient).
